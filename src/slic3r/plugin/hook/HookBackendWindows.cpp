#include "HookBackendWindows.hpp"
#include "HookMemory.hpp"
#include "HookRuntime.hpp"
#include <algorithm>
#include <array>
#include <atomic>
#include <cstring>
#include <filesystem>
#include <mutex>
#include <unordered_map>
#include <vector>
#include <cstdio>

#ifdef _WIN32
    #ifndef NOMINMAX
    #define NOMINMAX
    #endif
    #ifndef WIN32_LEAN_AND_MEAN
    #define WIN32_LEAN_AND_MEAN
    #endif
    #include <windows.h>
    #include <dbghelp.h>
    #include <psapi.h>
#endif

#if defined(_WIN32) && __has_include("safetyhook.hpp")
    #include "safetyhook.hpp"
    #define HOOK_HAS_SAFETYHOOK 1
#elif defined(_WIN32) && __has_include(<safetyhook/safetyhook.hpp>)
    #include <safetyhook/safetyhook.hpp>
    #define HOOK_HAS_SAFETYHOOK 1
#else
    #define HOOK_HAS_SAFETYHOOK 0
#endif

namespace Slic3r::Hook {

struct WindowsHookBackend::Impl {
#if HOOK_HAS_SAFETYHOOK
    struct Entry {
        enum class Kind { Inline, VTable, Trap } kind;
        HookHandle handle = nullptr;
        safetyhook::InlineHook inlineHook;
        void* vtable_slot_ptr = nullptr;
        void* original_vtable_value = nullptr;
        void* iat_ptr = nullptr;
        void* original_import = nullptr;
        size_t trap_slot = static_cast<size_t>(-1);
    };
    std::vector<std::unique_ptr<Entry>> entries;
#else
    struct Entry { void* handle=nullptr; void* target=nullptr; void* detour=nullptr; };
    std::vector<std::unique_ptr<Entry>> entries;
#endif
};


// ---- Breakpoint-trap sites (int3, 1 byte) for interior points ---------------
// A trap site patches exactly one byte with 0xCC and relocates nothing, so it
// stays correct where SafetyHook mid-hooks cannot go: functions whose
// prologue already carries an ENTRY patch, and code SafetyHook cannot safely
// steal (e.g. /GS cookie sequences). A process-wide VEH handler dispatches
// the chain and resumes: OFFSET re-executes the original instruction through
// a single-step dance, RETURN emulates the ret (see EmulateReturnResume).
// Slots are monotonic (never reused): once published, addr/orig/target are
// immutable, so the VEH handler can read them lock-free. Removal restores
// the byte and clears addr. Same-address installs share the slot (refcount).
namespace {
constexpr size_t kMaxTrapSitesWin = 256;
constexpr uint8_t kInt3 = 0xCC;
constexpr DWORD kTrapSingleStepFlag = 0x100; // EFLAGS.TF
struct TrapSiteWin {
    std::atomic<uint64_t> addr{0}; // 0 = empty/invalid
    uint8_t orig = 0;
    TargetInfo target{};
    std::atomic<int> refcount{0};
};
std::array<TrapSiteWin, kMaxTrapSitesWin> g_trap_sites_win;
std::atomic<size_t> g_trap_next_win{0};
std::mutex g_trap_alloc_mutex_win;
// Single-step dance state: site whose original byte is temporarily restored
// on this thread. At most one instruction ever runs between arm and disarm,
// so a single slot (not a stack) suffices; nested hits are sequential, not
// nested (the single-step trap is delivered before any later int3 executes).
thread_local uint64_t t_trap_armed_win = 0;

const TrapSiteWin* FindTrapSiteWin(uint64_t addr) {
    size_t n = g_trap_next_win.load(std::memory_order_acquire);
    if (n > kMaxTrapSitesWin) n = kMaxTrapSitesWin;
    for (size_t i = 0; i < n; ++i) {
        if (g_trap_sites_win[i].addr.load(std::memory_order_acquire) == addr)
            return &g_trap_sites_win[i];
    }
    return nullptr;
}

#define HOOK_TRAP_CTX_FROM(n) do { out.xmm##n.low = c->FltSave.XmmRegisters[n].Low; out.xmm##n.high = c->FltSave.XmmRegisters[n].High; } while (0)
#define HOOK_TRAP_CTX_TO(n) do { c->FltSave.XmmRegisters[n].Low = in.xmm##n.low; c->FltSave.XmmRegisters[n].High = in.xmm##n.high; } while (0)
CpuContext TrapCtxFromWin(CONTEXT* c, uint64_t site_addr) {
    CpuContext out = orca_hook::make_cpu_context();
    out.rax = c->Rax; out.rbx = c->Rbx; out.rcx = c->Rcx; out.rdx = c->Rdx;
    out.rsi = c->Rsi; out.rdi = c->Rdi; out.rbp = c->Rbp; out.rsp = c->Rsp;
    out.r8 = c->R8; out.r9 = c->R9; out.r10 = c->R10; out.r11 = c->R11;
    out.r12 = c->R12; out.r13 = c->R13; out.r14 = c->R14; out.r15 = c->R15;
    // Report the real code address, not a trampoline: unlike SafetyHook mids,
    // traps execute in place, so rip is directly meaningful to callbacks.
    out.rip = site_addr; out.rflags = c->EFlags;
    HOOK_TRAP_CTX_FROM(0); HOOK_TRAP_CTX_FROM(1); HOOK_TRAP_CTX_FROM(2); HOOK_TRAP_CTX_FROM(3);
    HOOK_TRAP_CTX_FROM(4); HOOK_TRAP_CTX_FROM(5); HOOK_TRAP_CTX_FROM(6); HOOK_TRAP_CTX_FROM(7);
    HOOK_TRAP_CTX_FROM(8); HOOK_TRAP_CTX_FROM(9); HOOK_TRAP_CTX_FROM(10); HOOK_TRAP_CTX_FROM(11);
    HOOK_TRAP_CTX_FROM(12); HOOK_TRAP_CTX_FROM(13); HOOK_TRAP_CTX_FROM(14); HOOK_TRAP_CTX_FROM(15);
    return out;
}
void TrapCtxToWin(const CpuContext& in, CONTEXT* c) {
    // GP regs, XMM and flags flow back; Rip/Rsp resume is set explicitly by
    // the caller (dance resume vs ret emulation), never blindly from ctx.
    c->Rax = in.rax; c->Rbx = in.rbx; c->Rcx = in.rcx; c->Rdx = in.rdx;
    c->Rsi = in.rsi; c->Rdi = in.rdi; c->Rbp = in.rbp;
    c->R8 = in.r8; c->R9 = in.r9; c->R10 = in.r10; c->R11 = in.r11;
    c->R12 = in.r12; c->R13 = in.r13; c->R14 = in.r14; c->R15 = in.r15;
    c->EFlags = static_cast<DWORD>(in.rflags);
    HOOK_TRAP_CTX_TO(8); HOOK_TRAP_CTX_TO(9); HOOK_TRAP_CTX_TO(10); HOOK_TRAP_CTX_TO(11);
    HOOK_TRAP_CTX_TO(12); HOOK_TRAP_CTX_TO(13); HOOK_TRAP_CTX_TO(14); HOOK_TRAP_CTX_TO(15);
}
void TrapRepatchWin(uint64_t addr) {
    // The site page is RX outside install/remove, so re-protect before writing.
    void* va = reinterpret_cast<void*>(addr);
    HookMemory::ScopedProtect prot(va, 1, MemProt::ReadWriteExecute);
    if (!prot.ok())
        return;
    uint8_t cc = kInt3;
    HookMemory::Write(va, &cc, 1);
    HookMemory::FlushICache(va, 1);
}
LONG CALLBACK TrapVehHandlerWin(EXCEPTION_POINTERS* ep) {
    DWORD code = ep->ExceptionRecord->ExceptionCode;
    CONTEXT* c = ep->ContextRecord;
    if (code == EXCEPTION_BREAKPOINT) {
        uint64_t addr = reinterpret_cast<uint64_t>(ep->ExceptionRecord->ExceptionAddress);
        const TrapSiteWin* site = FindTrapSiteWin(addr);
        if (site == nullptr)
            return EXCEPTION_CONTINUE_SEARCH; // foreign int3 (e.g. debugger): not ours
        TargetInfo target = site->target;
        CpuContext cctx = TrapCtxFromWin(c, addr);
        Slic3r::Hook::HookRuntime::Instance().GetDispatcher()->DispatchMid(target, &cctx);
        TrapCtxToWin(cctx, c);
        if (target.point == Slic3r::Hook::HookPoint::RETURN) {
            // Use the ORIGINAL byte plus following live bytes for decode.
            uint8_t bytes[16];
            bytes[0] = site->orig;
            SIZE_T got = 0;
            MEMORY_BASIC_INFORMATION mbi{};
            bool readable = VirtualQuery(reinterpret_cast<LPCVOID>(addr + 1), &mbi, sizeof(mbi)) != 0 &&
                            (mbi.State & MEM_COMMIT) != 0 && (mbi.Protect & (PAGE_EXECUTE | PAGE_EXECUTE_READ | PAGE_EXECUTE_READWRITE | PAGE_EXECUTE_WRITECOPY | PAGE_READONLY | PAGE_READWRITE | PAGE_WRITECOPY)) != 0;
            if (readable)
                readable = HookMemory::Read(reinterpret_cast<const void*>(addr + 1), bytes + 1, sizeof(bytes) - 1);
            uint64_t stacked = 0;
            if (readable)
                readable = HookMemory::Read(reinterpret_cast<const void*>(cctx.rsp), &stacked, sizeof(stacked));
            uint64_t rip = 0, rsp = 0;
            if (!readable || !Slic3r::Hook::EmulateReturnResume(bytes, sizeof(bytes), stacked, cctx.rsp, rip, rsp))
                return EXCEPTION_CONTINUE_SEARCH; // undecodable: fail honestly, never loop
            c->Rip = rip;
            c->Rsp = rsp;
        } else {
            // OFFSET single-step dance: restore the original byte, arm the
            // thread, resume AT the site so the instruction really executes.
            // The single-step trap then re-patches and continues past it.
            void* site_va = reinterpret_cast<void*>(addr);
            HookMemory::ScopedProtect dance_prot(site_va, 1, MemProt::ReadWriteExecute);
            if (!dance_prot.ok())
                return EXCEPTION_CONTINUE_SEARCH; // cannot restore: fail honestly
            HookMemory::Write(site_va, &site->orig, 1);
            HookMemory::FlushICache(site_va, 1);
            t_trap_armed_win = addr;
            // Honor a callback-redirected rip; default is the site itself.
            c->Rip = cctx.rip;
            c->Rsp = cctx.rsp;
            c->EFlags |= kTrapSingleStepFlag;
        }
        return EXCEPTION_CONTINUE_EXECUTION;
    }
    if (code == EXCEPTION_SINGLE_STEP) {
        if (t_trap_armed_win == 0)
            return EXCEPTION_CONTINUE_SEARCH; // foreign single-step (debugger): not ours
        TrapRepatchWin(t_trap_armed_win);
        t_trap_armed_win = 0;
        c->EFlags &= ~kTrapSingleStepFlag;
        return EXCEPTION_CONTINUE_EXECUTION; // Rip already past the stepped instruction
    }
    // Any other fault while armed (e.g. the stepped instruction itself
    // faulted): heal the site and let the fault continue honestly.
    if (t_trap_armed_win != 0) {
        TrapRepatchWin(t_trap_armed_win);
        t_trap_armed_win = 0;
        c->EFlags &= ~kTrapSingleStepFlag;
    }
    return EXCEPTION_CONTINUE_SEARCH;
}
void EnsureTrapVehWin() {
    static std::once_flag once;
    std::call_once(once, [] { AddVectoredExceptionHandler(1, &TrapVehHandlerWin); });
}
} // namespace

WindowsHookBackend::WindowsHookBackend() {
#ifdef _WIN32
    // Manifest RVAs and build_id are relative to OrcaSlicer.dll, not the exe.
    image_base_ = reinterpret_cast<void*>(GetModuleHandleW(L"OrcaSlicer.dll"));
    HMODULE loaded_dll = nullptr;
    if (!image_base_) {
        wchar_t exePath[MAX_PATH] = {0};
        if (GetModuleFileNameW(nullptr, exePath, MAX_PATH) > 0) {
            std::filesystem::path exe(exePath);
            std::vector<std::filesystem::path> candidates;
            candidates.push_back(exe.parent_path().parent_path() / "src" / "OrcaSlicer.dll");
            candidates.push_back(exe.parent_path() / "OrcaSlicer.dll");
            candidates.push_back(exe.parent_path().parent_path().parent_path() / "src" / "OrcaSlicer.dll");
            try {
                candidates.push_back(std::filesystem::absolute(std::filesystem::path(L"cmake-build-relwithdebinfo-visual-studio-llvm/src/OrcaSlicer.dll")));
            } catch (...) {}
            try {
                candidates.push_back(std::filesystem::absolute(std::filesystem::path(L"C:/Users/User/CLionProjects/OrcaSlicer/cmake-build-relwithdebinfo-visual-studio-llvm/src/OrcaSlicer.dll")));
            } catch (...) {}
            try {
                candidates.push_back(std::filesystem::absolute(std::filesystem::path(L"src/OrcaSlicer.dll")));
            } catch (...) {}
            {
                std::filesystem::path cur = exe.parent_path();
                for(int depth=0; depth<6 && !cur.empty(); ++depth){
                    std::filesystem::path dll_next = cur / "OrcaSlicer.dll";
                    if (std::find(candidates.begin(), candidates.end(), dll_next) == candidates.end())
                        candidates.push_back(dll_next);
                    std::filesystem::path dll_src = cur / "src" / "OrcaSlicer.dll";
                    if (std::find(candidates.begin(), candidates.end(), dll_src) == candidates.end())
                        candidates.push_back(dll_src);
                    auto parent = cur.parent_path();
                    if (parent == cur) break;
                    cur = parent;
                }
            }
            for (auto &cand : candidates) {
                std::error_code ec;
                std::filesystem::path abs_cand;
                try { abs_cand = std::filesystem::absolute(cand); } catch (...) { abs_cand = cand; }
                if (std::filesystem::exists(abs_cand, ec) && !ec) {
                    HMODULE h = LoadLibraryW(abs_cand.wstring().c_str());
                    if (h) {
                        image_base_ = reinterpret_cast<void*>(h);
                        loaded_dll = h;
                        fprintf(stderr, "[HookBackend] Fallback loaded OrcaSlicer.dll from %ls for build_id\n", abs_cand.wstring().c_str());
                        break;
                    } else {
                        fprintf(stderr, "[HookBackend] Fallback LoadLibrary failed for %ls err=%lu\n", abs_cand.wstring().c_str(), (unsigned long)GetLastError());
                    }
                }
            }
        }
        if (!image_base_) {
            fprintf(stderr, "[HookBackend] Fallback did not find OrcaSlicer.dll - build_id will be unavailable (no SKIP, will FAIL deterministically)\n");
            // Never fall back to the exe image: its build_id would mismatch.
        }
    }
    impl_ = new Impl();
    supported_ = (image_base_ != nullptr);
    InitBuildId();
    (void)loaded_dll;
#else
    impl_ = new Impl();
    supported_ = false;
    image_base_ = nullptr;
#endif
}

WindowsHookBackend::~WindowsHookBackend() { delete impl_; }

bool WindowsHookBackend::IsSupported() const noexcept {
#ifdef _WIN32
    return supported_ && image_base_ != nullptr;
#else
    return false;
#endif
}

void* WindowsHookBackend::ImageBase() const noexcept { return image_base_; }

void* WindowsHookBackend::RvaToVa(uint64_t rva) const noexcept {
    if (!image_base_) return nullptr;
    return reinterpret_cast<uint8_t*>(image_base_) + rva;
}

InstallResult WindowsHookBackend::MakeError(const std::string& msg) const {
    return InstallResult{false, msg, nullptr, nullptr, nullptr};
}
void WindowsHookBackend::InitBuildId() {
#ifdef _WIN32
    active_build_.os = 1;
    active_build_.arch = 1;
    active_build_.debug_file = "OrcaSlicer.dll";
    if (!image_base_) {
        fprintf(stderr, "[HookBackend] InitBuildId: image_base is null\n");
        return;
    }
    fprintf(stderr, "[HookBackend] InitBuildId: image_base=%p dll handle=%p exe handle=%p\n", image_base_, (void*)GetModuleHandleW(L"OrcaSlicer.dll"), (void*)GetModuleHandleW(nullptr));
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(image_base_);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) {
        fprintf(stderr, "[HookBackend] InitBuildId: bad dos magic %x\n", dos->e_magic);
        return;
    }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(image_base_) + dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) {
        fprintf(stderr, "[HookBackend] InitBuildId: bad nt signature\n");
        return;
    }
    auto& dbgDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_DEBUG];
    fprintf(stderr, "[HookBackend] InitBuildId: debug VA %lx size %lu\n", (unsigned long)dbgDir.VirtualAddress, (unsigned long)dbgDir.Size);
    if (dbgDir.VirtualAddress == 0 || dbgDir.Size == 0) {
        fprintf(stderr, "[HookBackend] InitBuildId: no debug dir\n");
        return;
    }
    auto* dbg = reinterpret_cast<IMAGE_DEBUG_DIRECTORY*>(reinterpret_cast<uint8_t*>(image_base_) + dbgDir.VirtualAddress);
    uint32_t n = dbgDir.Size / sizeof(IMAGE_DEBUG_DIRECTORY);
    fprintf(stderr, "[HookBackend] InitBuildId: debug entries %u\n", n);
    for (uint32_t i=0;i<n;++i){
        fprintf(stderr, "[HookBackend] debug[%u] Type=%u SizeOfData=%u\n", i, dbg[i].Type, dbg[i].SizeOfData);
        if (dbg[i].Type == IMAGE_DEBUG_TYPE_CODEVIEW){
            uint8_t* cv = reinterpret_cast<uint8_t*>(image_base_) + dbg[i].AddressOfRawData;
            fprintf(stderr, "[HookBackend] cv header %c%c%c%c\n", cv[0], cv[1], cv[2], cv[3]);
            if (dbg[i].SizeOfData >= 24 && cv[0]=='R' && cv[1]=='S' && cv[2]=='D' && cv[3]=='S'){
                memcpy(active_build_.debug_guid, cv+4, 16);
                active_build_.debug_age = *reinterpret_cast<uint32_t*>(cv+20);
                const char* p = reinterpret_cast<const char*>(cv+24);
                size_t max = dbg[i].SizeOfData - 24;
                size_t len = strnlen(p, max);
                active_build_.debug_file.assign(p, len);
                fprintf(stderr, "[HookBackend] guid copied, age %u file %s\n", active_build_.debug_age, active_build_.debug_file.c_str());
            }
            break;
        }
    }
    {
        HMODULE mod = reinterpret_cast<HMODULE>(image_base_);
        wchar_t wpath[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameW(mod, wpath, MAX_PATH);
        fprintf(stderr, "[HookBackend] GetModuleFileName len %lu\n", (unsigned long)len);
        if (len > 0 && len < MAX_PATH) {
            char pathA[MAX_PATH*2]={0};
            WideCharToMultiByte(CP_UTF8,0,wpath,len,pathA,sizeof(pathA),nullptr,nullptr);
            fprintf(stderr, "[HookBackend] path %s\n", pathA);
            auto compute_sha = [](const wchar_t* wpath, uint8_t out[32]) -> bool {
                FILE* f = _wfopen(wpath, L"rb");
                if (!f) return false;
                struct Ctx { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t buflen; };
                auto rotr = [](uint32_t x, uint32_t n){ return (x>>n)|(x<<(32-n)); };
                const uint32_t k256[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
                Ctx c; c.h[0]=0x6a09e667; c.h[1]=0xbb67ae85; c.h[2]=0x3c6ef372; c.h[3]=0xa54ff53a; c.h[4]=0x510e527f; c.h[5]=0x9b05688c; c.h[6]=0x1f83d9ab; c.h[7]=0x5be0cd19; c.len=0; c.buflen=0;
                auto compress = [&](const uint8_t* p){ uint32_t w[64]; for(int i=0;i<16;i++) w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]); for(int i=16;i<64;i++){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;} uint32_t a=c.h[0],b=c.h[1],cc=c.h[2],d=c.h[3],e=c.h[4],f=c.h[5],g=c.h[6],hh=c.h[7]; for(int i=0;i<64;i++){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25); uint32_t ch=(e&f)^((~e)&g); uint32_t temp1=hh+S1+ch+k256[i]+w[i]; uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22); uint32_t maj=(a&b)^(a&cc)^(b&cc); uint32_t temp2=S0+maj; hh=g; g=f; f=e; e=d+temp1; d=cc; cc=b; b=a; a=temp1+temp2;} c.h[0]+=a; c.h[1]+=b; c.h[2]+=cc; c.h[3]+=d; c.h[4]+=e; c.h[5]+=f; c.h[6]+=g; c.h[7]+=hh;};
                auto update = [&](const uint8_t* data, size_t len){ c.len+=len; while(len){ size_t need=64 - c.buflen; size_t take=len<need?len:need; memcpy(c.buf + c.buflen, data, take); c.buflen+=take; data+=take; len-=take; if(c.buflen==64){ compress(c.buf); c.buflen=0; } } };
                uint8_t buf[8192];
                size_t n;
                while ((n = fread(buf,1,sizeof(buf),f)) > 0) update(buf,n);
                fclose(f);
                uint64_t bitlen = c.len * 8;
                uint8_t pad[64]={0}; pad[0]=0x80;
                size_t padlen = c.buflen < 56 ? 56 - c.buflen : 120 - c.buflen;
                update(pad, padlen);
                uint8_t lenbytes[8]; for(int i=7;i>=0;--i){ lenbytes[i]=uint8_t(bitlen & 0xFF); bitlen>>=8; }
                update(lenbytes,8);
                for(int i=0;i<8;i++){ out[i*4+0]=uint8_t(c.h[i]>>24); out[i*4+1]=uint8_t(c.h[i]>>16); out[i*4+2]=uint8_t(c.h[i]>>8); out[i*4+3]=uint8_t(c.h[i]); }
                return true;
            };
            uint8_t hash[32] = {0};
            if (compute_sha(wpath, hash)) {
                memcpy(active_build_.image_hash, hash, 32);
                fprintf(stderr, "[HookBackend] hash computed\n");
            } else {
                fprintf(stderr, "[HookBackend] hash compute failed for %s\n", pathA);
            }
        } else {
            fprintf(stderr, "[HookBackend] GetModuleFileName failed len=%lu\n", (unsigned long)len);
        }
    }
#endif
}

BuildId WindowsHookBackend::ActiveBuildId() const { return active_build_; }
#ifdef _WIN32
static void* FindIATSlot(void* imageBase, const std::string& module, const std::string& symbol, std::string& err) {
    if (!imageBase) { err="null image base"; return nullptr; }
    auto* dos = reinterpret_cast<IMAGE_DOS_HEADER*>(imageBase);
    if (dos->e_magic != IMAGE_DOS_SIGNATURE) { err="bad dos"; return nullptr; }
    auto* nt = reinterpret_cast<IMAGE_NT_HEADERS64*>(reinterpret_cast<uint8_t*>(imageBase)+dos->e_lfanew);
    if (nt->Signature != IMAGE_NT_SIGNATURE) { err="bad nt"; return nullptr; }
    auto& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress==0){ err="no import dir"; return nullptr; }
    auto* imp = reinterpret_cast<IMAGE_IMPORT_DESCRIPTOR*>(reinterpret_cast<uint8_t*>(imageBase)+impDir.VirtualAddress);
    for (; imp->Name != 0; ++imp){
        const char* modName = reinterpret_cast<const char*>(reinterpret_cast<uint8_t*>(imageBase)+imp->Name);
        std::string mod(modName);
        std::transform(mod.begin(), mod.end(), mod.begin(), ::tolower);
        std::string want = module; std::transform(want.begin(), want.end(), want.begin(), ::tolower);
        if (want.empty() || mod.find(want) == std::string::npos) {
            if (!want.empty() && want != mod) continue;
        }
        uintptr_t* thunk = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(imageBase)+imp->FirstThunk);
        uintptr_t* orig  = nullptr;
        if (imp->OriginalFirstThunk) orig = reinterpret_cast<uintptr_t*>(reinterpret_cast<uint8_t*>(imageBase)+imp->OriginalFirstThunk);
        else orig = thunk;
        for (size_t idx=0; thunk[idx]!=0; ++idx){
            std::string curSym;
            if (orig[idx] & IMAGE_ORDINAL_FLAG64) curSym = "#" + std::to_string(IMAGE_ORDINAL64(orig[idx]));
            else { auto* ibn = reinterpret_cast<IMAGE_IMPORT_BY_NAME*>(reinterpret_cast<uint8_t*>(imageBase)+orig[idx]); curSym = ibn->Name; }
            if (curSym == symbol) return &thunk[idx];
        }
        if (!want.empty() && mod.find(want)!=std::string::npos) { err="symbol not found in module "+mod; return nullptr; }
    }
    err="IAT slot not found for "+module+"!"+symbol;
    return nullptr;
}
#endif

InstallResult WindowsHookBackend::InstallInline(const TargetInfo& target, void* detour) {
    if (!IsSupported()) return MakeError("backend not supported");
    if (!detour) return MakeError("null detour");
    void* va = target.absolute ? target.absolute : RvaToVa(target.rva);
    if (!va) return MakeError("null target va");
#if HOOK_HAS_SAFETYHOOK
    auto hook = safetyhook::create_inline(va, detour);
    if (!hook) return MakeError("safetyhook create_inline failed");
    auto* e = new Impl::Entry();
    e->kind = Impl::Entry::Kind::Inline;
    e->inlineHook = std::move(hook);
    e->handle = e;
    void* tramp = reinterpret_cast<void*>(e->inlineHook.trampoline().address());
    if (!tramp) tramp = e->inlineHook.original<void*>();
    InstallResult r; r.ok=true; r.handle=e; r.trampoline=tramp; r.error.clear();
    impl_->entries.emplace_back(e);
    return r;
#else
    (void)va;
    return MakeError("SafetyHook not available at build time (need deps/SafetyHook amalgamated header)");
#endif
}
bool WindowsHookBackend::ReadPristine(uint64_t rva, uint32_t size, std::vector<uint8_t>& out) const {
    out.clear();
    if (image_base_ == nullptr || size == 0)
        return false;
#ifdef _WIN32
    wchar_t wpath[MAX_PATH] = {0};
    DWORD len = GetModuleFileNameW(reinterpret_cast<HMODULE>(image_base_), wpath, MAX_PATH);
    if (len == 0 || len >= MAX_PATH)
        return false;
    FILE* f = _wfopen(wpath, L"rb");
    if (!f)
        return false;
    IMAGE_DOS_HEADER dos{};
    bool ok = fread(&dos, 1, sizeof(dos), f) == sizeof(dos) && dos.e_magic == IMAGE_DOS_SIGNATURE;
    IMAGE_NT_HEADERS64 nt{};
    if (ok)
        ok = _fseeki64(f, dos.e_lfanew, SEEK_SET) == 0 &&
             fread(&nt, 1, sizeof(nt), f) == sizeof(nt) && nt.Signature == IMAGE_NT_SIGNATURE;
    int64_t sec_off = static_cast<int64_t>(dos.e_lfanew) + 4 + 20 + nt.FileHeader.SizeOfOptionalHeader;
    if (ok)
        ok = _fseeki64(f, sec_off, SEEK_SET) == 0;
    DWORD raw_ptr = 0;
    if (ok) {
        ok = false;
        for (WORD i = 0; i < nt.FileHeader.NumberOfSections; ++i) {
            IMAGE_SECTION_HEADER sh{};
            if (fread(&sh, 1, sizeof(sh), f) != sizeof(sh))
                break;
            // File bytes only cover SizeOfRawData; callers decode code, which
            // always lives in raw data (no BSS decoding).
            if (rva >= sh.VirtualAddress && size <= sh.SizeOfRawData &&
                rva - sh.VirtualAddress <= sh.SizeOfRawData - size) {
                raw_ptr = sh.PointerToRawData + static_cast<DWORD>(rva - sh.VirtualAddress);
                ok = true;
                break;
            }
        }
    }
    if (ok)
        ok = _fseeki64(f, raw_ptr, SEEK_SET) == 0;
    out.resize(ok ? size : 0);
    if (ok)
        ok = fread(out.data(), 1, size, f) == size;
    fclose(f);
    if (!ok)
        out.clear();
    return ok;
#else
    (void)rva; (void)size;
    return false;
#endif
}
bool WindowsHookBackend::FindReturnOffsets(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (!IsSupported() || size == 0)
        return false;
    // Pristine bytes from the image file, not live memory: an ENTRY mid-hook
    // installed earlier in this process overwrites the prologue with a jump,
    // and decoding that jump desynchronizes the scan into spurious RET sites
    // that install cleanly but never fire (see FindReturnOffsets contract).
    std::vector<uint8_t> bytes;
    if (!ReadPristine(rva, size, bytes))
        return false;
    return ScanReturnOffsets(bytes.data(), bytes.size(), out);
}
bool WindowsHookBackend::FindInstructionBoundaries(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (!IsSupported() || size == 0)
        return false;
    std::vector<uint8_t> bytes;
    if (!ReadPristine(rva, size, bytes))
        return false;
    return ScanInstructionBoundaries(bytes.data(), bytes.size(), out);
}

InstallResult WindowsHookBackend::InstallTrap(const TargetInfo& target, uint32_t offset_within_symbol) {
    if (!IsSupported())
        return MakeError("backend not supported");
    if (target.point != HookPoint::ENTRY && target.point != HookPoint::OFFSET &&
        target.point != HookPoint::INVOKE && target.point != HookPoint::RETURN)
        return MakeError("traps serve in-function points (ENTRY/OFFSET/INVOKE/RETURN) only");
    if (target.size != 0 && offset_within_symbol >= target.size)
        return MakeError("offset out of range");
#ifdef _WIN32
    EnsureTrapVehWin();
    void* base = target.absolute ? target.absolute : RvaToVa(target.rva);
    if (!base)
        return MakeError("null base");
    auto* va = reinterpret_cast<uint8_t*>(base) + offset_within_symbol;
    uint64_t addr = reinterpret_cast<uint64_t>(va);
    // Same-address installs share the slot (e.g. an OFFSET at a RET site
    // plus a RETURN chain on the same target): both chains dispatch.
    {
        std::lock_guard<std::mutex> g(g_trap_alloc_mutex_win);
        size_t n = g_trap_next_win.load(std::memory_order_acquire);
        for (size_t i = 0; i < n && i < kMaxTrapSitesWin; ++i) {
            if (g_trap_sites_win[i].addr.load(std::memory_order_acquire) == addr) {
                g_trap_sites_win[i].refcount.fetch_add(1, std::memory_order_relaxed);
                auto* e = new Impl::Entry();
                e->kind = Impl::Entry::Kind::Trap;
                e->trap_slot = i;
                e->handle = e;
                InstallResult r;
                r.ok = true; r.handle = e;
                impl_->entries.emplace_back(e);
                return r;
            }
        }
        if (n >= kMaxTrapSitesWin)
            return MakeError("too many trap sites (max 256)");
        uint8_t orig = 0;
        if (!HookMemory::Read(va, &orig, 1))
            return MakeError("cannot read trap site");
        if (orig == kInt3)
            return MakeError("site already holds a breakpoint (foreign int3)");
        {
            HookMemory::ScopedProtect prot(va, 1, MemProt::ReadWriteExecute);
            if (!prot.ok())
                return MakeError("VirtualProtect failed for trap site");
            if (!HookMemory::Write(va, &kInt3, 1))
                return MakeError("cannot write trap site");
            HookMemory::FlushICache(va, 1);
        }
        // Publish AFTER the byte is patched: any executing int3 then implies
        // a visible slot (see Remove for the mirrored order).
        g_trap_sites_win[n].orig = orig;
        g_trap_sites_win[n].target = target;
        g_trap_sites_win[n].refcount.store(1, std::memory_order_relaxed);
        g_trap_sites_win[n].addr.store(addr, std::memory_order_release);
        g_trap_next_win.store(n + 1, std::memory_order_release);
        auto* e = new Impl::Entry();
        e->kind = Impl::Entry::Kind::Trap;
        e->trap_slot = n;
        e->handle = e;
        InstallResult r;
        r.ok = true; r.handle = e;
        impl_->entries.emplace_back(e);
        return r;
    }
#else
    (void)offset_within_symbol;
    return MakeError("traps not supported on this platform");
#endif
}

InstallResult WindowsHookBackend::InstallVTable(const TargetInfo& target, void* detour) {
    if (!IsSupported()) return MakeError("backend not supported");
    if (!detour) return MakeError("null detour");
    void* vt = target.absolute;
    if (!vt) vt = RvaToVa(target.rva);
    if (!vt) return MakeError("null vtable");
    uint32_t slot = target.vtable_slot;
    bool perInst = target.vtable_per_instance;
    void* inst = target.vtable_instance;
#ifdef _WIN32
    if (!perInst) {
        if (target.size != 0 && (slot + 1) * sizeof(void*) > target.size) return MakeError("vtable slot out of bounds (manifest)");
        void** slot_ptr = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vt) + slot * sizeof(void*));
        MEMORY_BASIC_INFORMATION mbi{};
        if (VirtualQuery(slot_ptr, &mbi, sizeof(mbi))==0) return MakeError("VirtualQuery failed for vtable slot");
        HookMemory::ScopedProtect prot(slot_ptr, sizeof(void*), MemProt::ReadWrite);
        if (!prot.ok()) return MakeError("VirtualProtect failed for vtable");
        void* orig = *slot_ptr;
        *slot_ptr = detour;
        HookMemory::FlushICache(slot_ptr, sizeof(void*));
#if HOOK_HAS_SAFETYHOOK
        auto* e = new Impl::Entry();
        e->kind = Impl::Entry::Kind::VTable;
        e->vtable_slot_ptr = slot_ptr;
        e->original_vtable_value = orig;
        e->handle = e;
        impl_->entries.emplace_back(e);
        InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig;
        return r;
#else
        InstallResult r; r.ok=true; r.handle=slot_ptr; r.trampoline=orig; r.original_import=orig;
        return r;
#endif
    } else {
        if (!inst) return MakeError("per-instance vtable requires instance pointer");
        if (target.size == 0) return MakeError("vtable size unknown (manifest missing)");
        if ((slot + 1) * sizeof(void*) > target.size) return MakeError("vtable slot out of bounds (manifest)");
        void** obj = reinterpret_cast<void**>(inst);
        void* old_vt = *obj;
        size_t bytes = target.size;
        void* new_vt = VirtualAlloc(nullptr, bytes, MEM_COMMIT|MEM_RESERVE, PAGE_READWRITE);
        if (!new_vt) return MakeError("VirtualAlloc failed for vtable clone");
        memcpy(new_vt, old_vt, bytes);
        void** new_slot = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(new_vt) + slot * sizeof(void*));
        void* orig = *new_slot;
        *new_slot = detour;
        {
            HookMemory::ScopedProtect p(obj, sizeof(void*), MemProt::ReadWrite);
            if (!p.ok()) { VirtualFree(new_vt,0,MEM_RELEASE); return MakeError("protect instance vptr failed"); }
            *obj = new_vt;
        }
        HookMemory::FlushICache(new_vt, bytes);
#if HOOK_HAS_SAFETYHOOK
        auto* e = new Impl::Entry();
        e->kind = Impl::Entry::Kind::VTable;
        e->vtable_slot_ptr = obj;
        e->original_vtable_value = old_vt;
        e->iat_ptr = new_vt;
        e->handle = e;
        impl_->entries.emplace_back(e);
        InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig;
        return r;
#else
        InstallResult r; r.ok=true; r.handle=obj; r.trampoline=orig;
        r.original_import = new_vt;
        return r;
#endif
    }
#else
    (void)slot; (void)perInst; (void)inst; (void)vt;
    return MakeError("vtable not supported on this platform");
#endif
}

InstallResult WindowsHookBackend::InstallIAT(const TargetInfo& target, void* detour) {
    if (!IsSupported()) return MakeError("backend not supported");
    if (!detour) return MakeError("null detour");
#ifdef _WIN32
    std::string mod = target.import_module;
    std::string sym = target.import_symbol;
    if (sym.empty()) return MakeError("IAT requires import_symbol");
    std::string err;
    void* slot = FindIATSlot(image_base_, mod, sym, err);
    if (!slot) return MakeError(err);
    HookMemory::ScopedProtect prot(slot, sizeof(void*), MemProt::ReadWrite);
    if (!prot.ok()) return MakeError("VirtualProtect failed for IAT slot");
    void* orig = *reinterpret_cast<void**>(slot);
    *reinterpret_cast<void**>(slot) = detour;
    HookMemory::FlushICache(slot, sizeof(void*));
#if HOOK_HAS_SAFETYHOOK
    auto* e = new Impl::Entry();
    e->kind = Impl::Entry::Kind::VTable;
    e->iat_ptr = slot;
    e->original_import = orig;
    e->handle = e;
    impl_->entries.emplace_back(e);
    InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig; r.original_import=orig;
    return r;
#else
    InstallResult r; r.ok=true; r.handle=slot; r.trampoline=orig; r.original_import=orig;
    return r;
#endif
#else
    (void)target; (void)detour;
    return MakeError("IAT not supported on this platform");
#endif
}

bool WindowsHookBackend::Remove(HookHandle handle, std::string& error) {
    if (!handle) { error="null handle"; return false; }
#if HOOK_HAS_SAFETYHOOK
    for (auto it = impl_->entries.begin(); it != impl_->entries.end(); ++it){
        if ((*it)->handle == handle || (*it).get() == handle){
            auto* e = it->get();
            bool ok = true;
            switch (e->kind){
                case Impl::Entry::Kind::Inline: e->inlineHook.reset(); break;
                case Impl::Entry::Kind::Trap: {
#ifdef _WIN32
                    size_t slot = e->trap_slot;
                    if (slot < kMaxTrapSitesWin) {
                        int left = g_trap_sites_win[slot].refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                        if (left <= 0) {
                            // Restore the byte BEFORE invalidating: a faulting
                            // thread then either sees no int3 (no trap) or a
                            // still-valid slot (mirrors install order).
                            void* va = reinterpret_cast<void*>(g_trap_sites_win[slot].addr.load(std::memory_order_acquire));
                            uint8_t orig = g_trap_sites_win[slot].orig;
                            if (va) {
                                HookMemory::ScopedProtect p(va, 1, MemProt::ReadWriteExecute);
                                if (p.ok()) {
                                    HookMemory::Write(va, &orig, 1);
                                    HookMemory::FlushICache(va, 1);
                                } else ok = false;
                            }
                            g_trap_sites_win[slot].addr.store(0, std::memory_order_release);
                        }
                    }
                    break;
#else
                    break;
#endif
                }
                case Impl::Entry::Kind::VTable: {
                    if (e->vtable_slot_ptr){
                        if (e->iat_ptr && e->vtable_slot_ptr){
                            void** obj = reinterpret_cast<void**>(e->vtable_slot_ptr);
                            void* clone = e->iat_ptr;
                            HookMemory::ScopedProtect p(obj, sizeof(void*), MemProt::ReadWrite);
                            if (p.ok()) *obj = e->original_vtable_value;
                            VirtualFree(clone, 0, MEM_RELEASE);
                        } else if (e->vtable_slot_ptr && e->original_vtable_value){
                            HookMemory::ScopedProtect p(e->vtable_slot_ptr, sizeof(void*), MemProt::ReadWrite);
                            if (p.ok()){
                                *reinterpret_cast<void**>(e->vtable_slot_ptr) = e->original_vtable_value;
                                HookMemory::FlushICache(e->vtable_slot_ptr, sizeof(void*));
                            } else ok=false;
                        }
                    } else if (e->iat_ptr && e->original_import){
                        HookMemory::ScopedProtect p(e->iat_ptr, sizeof(void*), MemProt::ReadWrite);
                        if (p.ok()){
                            *reinterpret_cast<void**>(e->iat_ptr) = e->original_import;
                            HookMemory::FlushICache(e->iat_ptr, sizeof(void*));
                        } else ok=false;
                    }
                    break;
                }
            }
            impl_->entries.erase(it);
            if (!ok){ error="protect failed during remove"; return false; }
            return true;
        }
    }
    error="handle not found";
    return false;
#else
    (void)handle; error="SafetyHook not available - cannot remove"; return false;
#endif
}

} // namespace Slic3r::Hook
