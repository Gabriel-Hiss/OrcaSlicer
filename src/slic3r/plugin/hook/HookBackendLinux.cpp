#include "HookBackendLinux.hpp"
#include "HookMemory.hpp"
#include "HookRuntime.hpp"

#include <algorithm>
#include <array>
#include <cstring>
#include <mutex>
#include <vector>
#include <string>
#include <cstdio>
#include <atomic>

#if defined(__linux__)
#  include <link.h>
#  include <dlfcn.h>
#if defined(__linux__) && defined(__x86_64__)
#include <signal.h>
#include <sys/ucontext.h>
#endif
#  include <unistd.h>
#  include <sys/mman.h>
#  include <elf.h>
#endif

#if defined(__linux__) && __has_include("safetyhook.hpp")
#  include "safetyhook.hpp"
#  define HOOK_HAS_SAFETYHOOK_LINUX 1
#elif defined(__linux__) && __has_include(<safetyhook/safetyhook.hpp>)
#  include <safetyhook/safetyhook.hpp>
#  define HOOK_HAS_SAFETYHOOK_LINUX 1
#else
#  define HOOK_HAS_SAFETYHOOK_LINUX 0
#endif

namespace Slic3r::Hook {

struct LinuxHookBackend::Impl {
#if HOOK_HAS_SAFETYHOOK_LINUX
    struct Entry {
        enum class Kind { Inline, VTable, Got, Trap } kind;
        HookHandle handle = nullptr;
        safetyhook::InlineHook inlineHook;
        void* got_slot_ptr = nullptr;
        void* original_got_value = nullptr;
        void* vtable_slot_ptr = nullptr;
        void* original_vtable_value = nullptr;
        void* cloned_vtable = nullptr;
        size_t cloned_count = 0;
        void* instance_ptr = nullptr;
        void* original_instance_vtable = nullptr;
        size_t trap_slot = static_cast<size_t>(-1);
    };
    std::vector<std::unique_ptr<Entry>> entries;
#else
    struct Entry { void* handle=nullptr; void* target=nullptr; void* detour=nullptr; };
    std::vector<std::unique_ptr<Entry>> entries;
#endif
};
#if HOOK_HAS_SAFETYHOOK_LINUX

// Breakpoint-trap sites (int3); same contract as the Windows backend.
#if defined(__linux__) && defined(__x86_64__)
namespace {
constexpr size_t kMaxTrapSitesLinux = 256;
constexpr uint8_t kInt3Linux = 0xCC;
constexpr greg_t kTrapSingleStepFlagLinux = 0x100; // EFLAGS.TF
struct TrapSiteLinux {
    std::atomic<uint64_t> addr{0};
    uint8_t orig = 0;
    TargetInfo target{};
    std::atomic<int> refcount{0};
};
std::array<TrapSiteLinux, kMaxTrapSitesLinux> g_trap_sites_linux;
std::atomic<size_t> g_trap_next_linux{0};
std::mutex g_trap_alloc_mutex_linux;
thread_local uint64_t t_trap_armed_linux = 0;

const TrapSiteLinux* FindTrapSiteLinux(uint64_t addr) {
    size_t n = g_trap_next_linux.load(std::memory_order_acquire);
    if (n > kMaxTrapSitesLinux) n = kMaxTrapSitesLinux;
    for (size_t i = 0; i < n; ++i) {
        if (g_trap_sites_linux[i].addr.load(std::memory_order_acquire) == addr)
            return &g_trap_sites_linux[i];
    }
    return nullptr;
}

CpuContext TrapCtxFromLinux(mcontext_t& m, uint64_t site_addr) {
    CpuContext out = orca_hook::make_cpu_context();
    const gregset_t& g = m.gregs;
    out.rax = (uint64_t)g[REG_RAX]; out.rbx = (uint64_t)g[REG_RBX];
    out.rcx = (uint64_t)g[REG_RCX]; out.rdx = (uint64_t)g[REG_RDX];
    out.rsi = (uint64_t)g[REG_RSI]; out.rdi = (uint64_t)g[REG_RDI];
    out.rbp = (uint64_t)g[REG_RBP]; out.rsp = (uint64_t)g[REG_RSP];
    out.r8 = (uint64_t)g[REG_R8]; out.r9 = (uint64_t)g[REG_R9];
    out.r10 = (uint64_t)g[REG_R10]; out.r11 = (uint64_t)g[REG_R11];
    out.r12 = (uint64_t)g[REG_R12]; out.r13 = (uint64_t)g[REG_R13];
    out.r14 = (uint64_t)g[REG_R14]; out.r15 = (uint64_t)g[REG_R15];
    out.rip = site_addr; out.rflags = (uint64_t)g[REG_EFL];
    auto* fp = m.fpregs;
    if (fp) {
        // Each _xmm entry is 16 bytes (low, high); memcpy keeps this robust
        // against glibc member-layout differences.
        uint64_t* dst[16][2] = {
            {&out.xmm0.low, &out.xmm0.high}, {&out.xmm1.low, &out.xmm1.high},
            {&out.xmm2.low, &out.xmm2.high}, {&out.xmm3.low, &out.xmm3.high},
            {&out.xmm4.low, &out.xmm4.high}, {&out.xmm5.low, &out.xmm5.high},
            {&out.xmm6.low, &out.xmm6.high}, {&out.xmm7.low, &out.xmm7.high},
            {&out.xmm8.low, &out.xmm8.high}, {&out.xmm9.low, &out.xmm9.high},
            {&out.xmm10.low, &out.xmm10.high}, {&out.xmm11.low, &out.xmm11.high},
            {&out.xmm12.low, &out.xmm12.high}, {&out.xmm13.low, &out.xmm13.high},
            {&out.xmm14.low, &out.xmm14.high}, {&out.xmm15.low, &out.xmm15.high},
        };
        for (int i = 0; i < 16; ++i) {
            uint8_t raw[16];
            std::memcpy(raw, &fp->_xmm[i], sizeof(raw));
            std::memcpy(dst[i][0], raw, 8);
            std::memcpy(dst[i][1], raw + 8, 8);
        }
    }
    return out;
}
void TrapCtxToLinux(const CpuContext& in, mcontext_t& m) {
    gregset_t& g = m.gregs;
    g[REG_RAX] = (greg_t)in.rax; g[REG_RBX] = (greg_t)in.rbx;
    g[REG_RCX] = (greg_t)in.rcx; g[REG_RDX] = (greg_t)in.rdx;
    g[REG_RSI] = (greg_t)in.rsi; g[REG_RDI] = (greg_t)in.rdi;
    g[REG_RBP] = (greg_t)in.rbp;
    g[REG_R8] = (greg_t)in.r8; g[REG_R9] = (greg_t)in.r9;
    g[REG_R10] = (greg_t)in.r10; g[REG_R11] = (greg_t)in.r11;
    g[REG_R12] = (greg_t)in.r12; g[REG_R13] = (greg_t)in.r13;
    g[REG_R14] = (greg_t)in.r14; g[REG_R15] = (greg_t)in.r15;
    g[REG_EFL] = (greg_t)in.rflags;
    auto* fp = m.fpregs;
    if (fp) {
        for (int i = 0; i < 16; ++i) {
            uint64_t* src[2] = {nullptr, nullptr};
            switch (i) {
                case 0: src[0] = (uint64_t*)&in.xmm0.low; src[1] = (uint64_t*)&in.xmm0.high; break;
                case 1: src[0] = (uint64_t*)&in.xmm1.low; src[1] = (uint64_t*)&in.xmm1.high; break;
                case 2: src[0] = (uint64_t*)&in.xmm2.low; src[1] = (uint64_t*)&in.xmm2.high; break;
                case 3: src[0] = (uint64_t*)&in.xmm3.low; src[1] = (uint64_t*)&in.xmm3.high; break;
                case 4: src[0] = (uint64_t*)&in.xmm4.low; src[1] = (uint64_t*)&in.xmm4.high; break;
                case 5: src[0] = (uint64_t*)&in.xmm5.low; src[1] = (uint64_t*)&in.xmm5.high; break;
                case 6: src[0] = (uint64_t*)&in.xmm6.low; src[1] = (uint64_t*)&in.xmm6.high; break;
                case 7: src[0] = (uint64_t*)&in.xmm7.low; src[1] = (uint64_t*)&in.xmm7.high; break;
                case 8: src[0] = (uint64_t*)&in.xmm8.low; src[1] = (uint64_t*)&in.xmm8.high; break;
                case 9: src[0] = (uint64_t*)&in.xmm9.low; src[1] = (uint64_t*)&in.xmm9.high; break;
                case 10: src[0] = (uint64_t*)&in.xmm10.low; src[1] = (uint64_t*)&in.xmm10.high; break;
                case 11: src[0] = (uint64_t*)&in.xmm11.low; src[1] = (uint64_t*)&in.xmm11.high; break;
                case 12: src[0] = (uint64_t*)&in.xmm12.low; src[1] = (uint64_t*)&in.xmm12.high; break;
                case 13: src[0] = (uint64_t*)&in.xmm13.low; src[1] = (uint64_t*)&in.xmm13.high; break;
                case 14: src[0] = (uint64_t*)&in.xmm14.low; src[1] = (uint64_t*)&in.xmm14.high; break;
                default: src[0] = (uint64_t*)&in.xmm15.low; src[1] = (uint64_t*)&in.xmm15.high; break;
            }
            uint8_t raw[16];
            std::memcpy(raw, src[0], 8);
            std::memcpy(raw + 8, src[1], 8);
            std::memcpy(&fp->_xmm[i], raw, sizeof(raw));
        }
    }
}
void TrapRepatchLinux(uint64_t addr) {
    void* va = reinterpret_cast<void*>(addr);
    HookMemory::ScopedProtect prot(va, 1, MemProt::ReadWriteExecute);
    if (!prot.ok())
        return;
    uint8_t cc = kInt3Linux;
    HookMemory::Write(va, &cc, 1);
    HookMemory::FlushICache(va, 1);
}
void TrapSigHandlerLinux(int sig, siginfo_t* info, void* uctx) {
    if (sig != SIGTRAP || !info || !uctx)
        return;
    auto* uc = static_cast<ucontext_t*>(uctx);
    mcontext_t& m = uc->uc_mcontext;
    if (info->si_code == TRAP_BRPT) {
        uint64_t addr = reinterpret_cast<uint64_t>(info->si_addr);
        if (addr == 0)
            addr = (uint64_t)m.gregs[REG_RIP] - 1; // int3 advances Rip past itself
        const TrapSiteLinux* site = FindTrapSiteLinux(addr);
        if (site == nullptr)
            return; // foreign breakpoint: leave default disposition
        TargetInfo target = site->target;
        CpuContext cctx = TrapCtxFromLinux(m, addr);
        Slic3r::Hook::HookRuntime::Instance().GetDispatcher()->DispatchMid(target, &cctx);
        TrapCtxToLinux(cctx, m);
        if (target.point == Slic3r::Hook::HookPoint::RETURN) {
            uint8_t bytes[16];
            bytes[0] = site->orig;
            bool ok = HookMemory::Read(reinterpret_cast<const void*>(addr + 1), bytes + 1, sizeof(bytes) - 1);
            uint64_t stacked = 0;
            if (ok)
                ok = HookMemory::Read(reinterpret_cast<const void*>(cctx.rsp), &stacked, sizeof(stacked));
            uint64_t rip = 0, rsp = 0;
            if (!ok || !Slic3r::Hook::EmulateReturnResume(bytes, sizeof(bytes), stacked, cctx.rsp, rip, rsp))
                return; // undecodable: leave disposition (honest fault, never loop)
            m.gregs[REG_RIP] = (greg_t)rip;
            m.gregs[REG_RSP] = (greg_t)rsp;
        } else {
            void* site_va = reinterpret_cast<void*>(addr);
            HookMemory::ScopedProtect dance_prot(site_va, 1, MemProt::ReadWriteExecute);
            if (!dance_prot.ok())
                return; // cannot restore: leave disposition (honest fault)
            HookMemory::Write(site_va, &site->orig, 1);
            HookMemory::FlushICache(site_va, 1);
            t_trap_armed_linux = addr;
            m.gregs[REG_RIP] = (greg_t)cctx.rip;
            m.gregs[REG_RSP] = (greg_t)cctx.rsp;
            m.gregs[REG_EFL] |= kTrapSingleStepFlagLinux;
        }
        return;
    }
    if (info->si_code == TRAP_TRACE) {
        if (t_trap_armed_linux == 0)
            return; // foreign single-step
        TrapRepatchLinux(t_trap_armed_linux);
        t_trap_armed_linux = 0;
        m.gregs[REG_EFL] &= ~kTrapSingleStepFlagLinux;
        return;
    }
    if (t_trap_armed_linux != 0) {
        TrapRepatchLinux(t_trap_armed_linux);
        t_trap_armed_linux = 0;
        m.gregs[REG_EFL] &= ~kTrapSingleStepFlagLinux;
    }
}
void EnsureTrapSigLinux() {
    static std::once_flag once;
    std::call_once(once, [] {
        struct sigaction sa{};
        sa.sa_sigaction = &TrapSigHandlerLinux;
        sigemptyset(&sa.sa_mask);
        sa.sa_flags = SA_SIGINFO | SA_RESTART;
        sigaction(SIGTRAP, &sa, nullptr);
    });
}
} // namespace
#endif
} // namespace

#endif
LinuxHookBackend::LinuxHookBackend() {
#if defined(__linux__) && defined(__x86_64__)
    Dl_info di{};
    auto* probe_fn = +[]() noexcept {};
    void* probe = reinterpret_cast<void*>(probe_fn);
    bool dl_ok = false;
    if (dladdr(probe, &di) && di.dli_fbase) {
        image_base_ = di.dli_fbase;
        dl_ok = true;
    }
    if (!dl_ok) image_base_ = nullptr;
    impl_ = new Impl();
    supported_ = true;
    InitBuildId();
#else
    impl_ = new Impl();
    supported_ = false;
    image_base_ = nullptr;
#endif
}

LinuxHookBackend::~LinuxHookBackend() { delete impl_; }

bool LinuxHookBackend::IsSupported() const noexcept {
#if defined(__linux__) && defined(__x86_64__)
    return supported_ && image_base_ != nullptr;
#else
    return false;
#endif
}

void* LinuxHookBackend::ImageBase() const noexcept { return image_base_; }

void* LinuxHookBackend::RvaToVa(uint64_t rva) const noexcept {
    if (!image_base_) return nullptr;
    return reinterpret_cast<uint8_t*>(image_base_) + rva;
}

InstallResult LinuxHookBackend::MakeError(const std::string& msg) const {
    return InstallResult{false, msg, nullptr, nullptr, nullptr};
}

void LinuxHookBackend::InitBuildId() {
#if defined(__linux__) && defined(__x86_64__)
    active_build_.os = 2;
    active_build_.arch = 1;
    active_build_.debug_file = "orca-slicer";
    if (!image_base_) return;
    auto* eh = reinterpret_cast<Elf64_Ehdr*>(image_base_);
    if (eh->e_ident[0]!=0x7f || eh->e_ident[1]!='E' || eh->e_ident[2]!='L' || eh->e_ident[3]!='F') return;
    if (eh->e_ident[EI_CLASS]!=ELFCLASS64) return;
    auto* phdrs = reinterpret_cast<Elf64_Phdr*>(reinterpret_cast<uint8_t*>(image_base_) + eh->e_phoff);
    for (int i=0;i<eh->e_phnum;++i){
        if (phdrs[i].p_type == PT_NOTE){
            uint8_t* data = reinterpret_cast<uint8_t*>(image_base_) + phdrs[i].p_offset;
            uint8_t* base = reinterpret_cast<uint8_t*>(image_base_) + phdrs[i].p_vaddr;
            for (int attempt=0; attempt<2; ++attempt){
                uint8_t* cur_data = (attempt==0? data : base);
                size_t sz = (size_t)phdrs[i].p_filesz;
                size_t cur=0;
                bool found=false;
                while (cur + sizeof(Elf64_Nhdr) <= sz){
                    auto* nh = reinterpret_cast<Elf64_Nhdr*>(cur_data+cur);
                    uint32_t namesz=nh->n_namesz, descsz=nh->n_descsz, type=nh->n_type;
                    size_t name_off = cur + sizeof(Elf64_Nhdr);
                    size_t desc_off = name_off + ((namesz+3)&~3u);
                    size_t next = desc_off + ((descsz+3)&~3u);
                    if (next > sz) break;
                    std::string name;
                    if (namesz>0 && name_off+namesz<=sz) name.assign(reinterpret_cast<char*>(cur_data+name_off), namesz);
                    if (type==NT_GNU_BUILD_ID && name.rfind("GNU",0)==0 && desc_off+descsz<=sz){
                        std::string hex;
                        hex.reserve(descsz*2);
                        const uint8_t* d = cur_data+desc_off;
                        char buf[3];
                        for (size_t k=0;k<descsz;++k){ snprintf(buf,sizeof(buf),"%02x", d[k]); hex+=buf; }
                        active_build_.gnu_build_id = hex;
                        size_t cpy = std::min<size_t>(16, descsz);
                        memcpy(active_build_.debug_guid, d, cpy);
                        found=true;
                        break;
                    }
                    cur = next;
                }
                if(found) break;
            }
        }
    }
    char exe_path[512]={0};
    ssize_t len = readlink("/proc/self/exe", exe_path, sizeof(exe_path)-1);
    if (len>0){
        exe_path[len]='\0';
        active_build_.debug_file = exe_path;
        auto compute_sha = [](const char* path, uint8_t out[32]) -> bool {
            FILE* f = fopen(path, "rb");
            if (!f) return false;
            struct Ctx { uint32_t h[8]; uint64_t len; uint8_t buf[64]; size_t buflen; };
            auto rotr = [](uint32_t x, uint32_t n){ return (x>>n)|(x<<(32-n)); };
            const uint32_t k256[64] = {0x428a2f98,0x71374491,0xb5c0fbcf,0xe9b5dba5,0x3956c25b,0x59f111f1,0x923f82a4,0xab1c5ed5,0xd807aa98,0x12835b01,0x243185be,0x550c7dc3,0x72be5d74,0x80deb1fe,0x9bdc06a7,0xc19bf174,0xe49b69c1,0xefbe4786,0x0fc19dc6,0x240ca1cc,0x2de92c6f,0x4a7484aa,0x5cb0a9dc,0x76f988da,0x983e5152,0xa831c66d,0xb00327c8,0xbf597fc7,0xc6e00bf3,0xd5a79147,0x06ca6351,0x14292967,0x27b70a85,0x2e1b2138,0x4d2c6dfc,0x53380d13,0x650a7354,0x766a0abb,0x81c2c92e,0x92722c85,0xa2bfe8a1,0xa81a664b,0xc24b8b70,0xc76c51a3,0xd192e819,0xd6990624,0xf40e3585,0x106aa070,0x19a4c116,0x1e376c08,0x2748774c,0x34b0bcb5,0x391c0cb3,0x4ed8aa4a,0x5b9cca4f,0x682e6ff3,0x748f82ee,0x78a5636f,0x84c87814,0x8cc70208,0x90befffa,0xa4506ceb,0xbef9a3f7,0xc67178f2};
            Ctx c; c.h[0]=0x6a09e667; c.h[1]=0xbb67ae85; c.h[2]=0x3c6ef372; c.h[3]=0xa54ff53a; c.h[4]=0x510e527f; c.h[5]=0x9b05688c; c.h[6]=0x1f83d9ab; c.h[7]=0x5be0cd19; c.len=0; c.buflen=0;
            auto compress = [&](const uint8_t* p){ uint32_t w[64]; for(int i=0;i<16;i++) w[i]=(uint32_t(p[i*4])<<24)|(uint32_t(p[i*4+1])<<16)|(uint32_t(p[i*4+2])<<8)|uint32_t(p[i*4+3]); for(int i=16;i<64;i++){uint32_t s0=rotr(w[i-15],7)^rotr(w[i-15],18)^(w[i-15]>>3); uint32_t s1=rotr(w[i-2],17)^rotr(w[i-2],19)^(w[i-2]>>10); w[i]=w[i-16]+s0+w[i-7]+s1;} uint32_t a=c.h[0],b=c.h[1],cc=c.h[2],d=c.h[3],e=c.h[4],f=c.h[5],g=c.h[6],hh=c.h[7]; for(int i=0;i<64;i++){uint32_t S1=rotr(e,6)^rotr(e,11)^rotr(e,25); uint32_t ch=(e&f)^((~e)&g); uint32_t temp1=hh+S1+ch+k256[i]+w[i]; uint32_t S0=rotr(a,2)^rotr(a,13)^rotr(a,22); uint32_t maj=(a&b)^(a&cc)^(b&cc); uint32_t temp2=S0+maj; hh=g; g=f; f=e; e=d+temp1; d=cc; cc=b; b=a; a=temp1+temp2;} c.h[0]+=a; c.h[1]+=b; c.h[2]+=cc; c.h[3]+=d; c.h[4]+=e; c.h[5]+=f; c.h[6]+=g; c.h[7]+=hh; };
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
        uint8_t hash[32]={0};
        if (compute_sha(exe_path, hash)) memcpy(active_build_.image_hash, hash, 32);
    }
#endif
}

BuildId LinuxHookBackend::ActiveBuildId() const { return active_build_; }

#if defined(__linux__) && defined(__x86_64__)
static void* FindGotSlotInMemory(void* imageBase, const std::string& module, const std::string& symbol, std::string& err){
    if (!imageBase){ err="null image base"; return nullptr; }
    auto* eh = reinterpret_cast<Elf64_Ehdr*>(imageBase);
    if (eh->e_ident[0]!=0x7f || eh->e_ident[1]!='E' || eh->e_ident[2]!='L' || eh->e_ident[3]!='F'){ err="bad elf magic"; return nullptr; }
    Elf64_Phdr* phdrs = reinterpret_cast<Elf64_Phdr*>(reinterpret_cast<uint8_t*>(imageBase)+eh->e_phoff);
    Elf64_Dyn* dyn = nullptr;
    size_t dyn_count=0;
    for(int i=0;i<eh->e_phnum;++i){
        if(phdrs[i].p_type==PT_DYNAMIC){
            if(eh->e_type==ET_DYN) dyn = reinterpret_cast<Elf64_Dyn*>(reinterpret_cast<uint8_t*>(imageBase)+phdrs[i].p_vaddr);
            else dyn = reinterpret_cast<Elf64_Dyn*>(reinterpret_cast<uint8_t*>(imageBase)+phdrs[i].p_vaddr);
            dyn_count = phdrs[i].p_filesz / sizeof(Elf64_Dyn);
            break;
        }
    }
    if(!dyn){ err="no PT_DYNAMIC"; return nullptr; }
    uint64_t jmprel_va=0, jmprel_sz=0, strtab_va=0, symtab_va=0;
    uint64_t rela_va=0, rela_sz=0;
    for(size_t i=0;i<dyn_count;++i){
        switch(dyn[i].d_tag){
            case DT_JMPREL: jmprel_va = dyn[i].d_un.d_ptr; break;
            case DT_PLTRELSZ: jmprel_sz = dyn[i].d_un.d_val; break;
            case DT_STRTAB: strtab_va = dyn[i].d_un.d_ptr; break;
            case DT_SYMTAB: symtab_va = dyn[i].d_un.d_ptr; break;
            case DT_RELA: rela_va = dyn[i].d_un.d_ptr; break;
            case DT_RELASZ: rela_sz = dyn[i].d_un.d_val; break;
            default: break;
        }
    }
    auto va_to_ptr = [&](uint64_t va)->void*{
        if(eh->e_type==ET_DYN) return reinterpret_cast<uint8_t*>(imageBase)+va;
        return reinterpret_cast<void*>(va);
    };
    std::string want_l = symbol; std::transform(want_l.begin(), want_l.end(), want_l.begin(), ::tolower);
    auto scan_relocs = [&](uint64_t base_va, uint64_t sz, bool is_jmprel)->void*{
        if(base_va==0 || sz==0) return nullptr;
        void* base_ptr = va_to_ptr(base_va);
        if(!base_ptr) return nullptr;
        size_t count = sz / sizeof(Elf64_Rela);
        for(size_t idx=0; idx<count; ++idx){
            Elf64_Rela* re = reinterpret_cast<Elf64_Rela*>(reinterpret_cast<uint8_t*>(base_ptr)+idx*sizeof(Elf64_Rela));
            uint32_t sym_idx = (uint32_t)(re->r_info >> 32);
            uint32_t type = (uint32_t)(re->r_info & 0xffffffffu);
            if(symtab_va==0 || strtab_va==0) continue;
            Elf64_Sym* sym = reinterpret_cast<Elf64_Sym*>(reinterpret_cast<uint8_t*>(va_to_ptr(symtab_va)) + sym_idx*sizeof(Elf64_Sym));
            char* strtab = reinterpret_cast<char*>(va_to_ptr(strtab_va));
            if(!sym || !strtab) continue;
            const char* name = strtab + sym->st_name;
            std::string cur(name? name:"");
            std::string cur_l = cur; std::transform(cur_l.begin(), cur_l.end(), cur_l.begin(), ::tolower);
            if(cur_l==want_l){
                if(is_jmprel && type!=R_X86_64_JUMP_SLOT) continue;
                void* slot = nullptr;
                if(eh->e_type==ET_DYN) slot = reinterpret_cast<uint8_t*>(imageBase)+re->r_offset;
                else slot = reinterpret_cast<void*>(re->r_offset);
                (void)module;
                return slot;
            }
        }
        return nullptr;
    };
    void* slot = scan_relocs(jmprel_va, jmprel_sz, true);
    if(slot) return slot;
    slot = scan_relocs(rela_va, rela_sz, false);
    if(slot) return slot;
    err="GOT slot not found for "+module+"!"+symbol;
    return nullptr;
}
#endif

void* LinuxHookBackend::FindGotSlot(const std::string& module, const std::string& symbol, std::string& err) const {
#if defined(__linux__) && defined(__x86_64__)
    if(!image_base_){ err="no image base"; return nullptr; }
    void* base = image_base_;
    void* handle = nullptr;
    if(!module.empty()){
        handle = dlopen(module.c_str(), RTLD_LAZY | RTLD_NOLOAD);
        if(handle){
            struct link_map* lm=nullptr;
            if(dlinfo(handle, RTLD_DI_LINKMAP, &lm)==0 && lm){
                base = reinterpret_cast<void*>(lm->l_addr);
            }
        }
    }
    void* slot = FindGotSlotInMemory(base, module, symbol, err);
    if(handle) dlclose(handle);
    return slot;
#else
    (void)module; (void)symbol; err="not on Linux x64";
    return nullptr;
#endif
}

InstallResult LinuxHookBackend::InstallInline(const TargetInfo& target, void* detour){
    if(!IsSupported()) return MakeError("backend not supported (Linux x64 required)");
    if(!detour) return MakeError("null detour");
    void* va = target.absolute ? target.absolute : RvaToVa(target.rva);
    if(!va) return MakeError("null target va");
#if HOOK_HAS_SAFETYHOOK_LINUX
    auto hook = safetyhook::create_inline(va, detour);
    if(!hook) return MakeError("safetyhook create_inline failed");
    auto* e = new Impl::Entry();
    e->kind = Impl::Entry::Kind::Inline;
    e->inlineHook = std::move(hook);
    e->handle = e;
    void* tramp = reinterpret_cast<void*>(e->inlineHook.trampoline().address());
    if (!tramp) tramp = e->inlineHook.original<void*>();
    InstallResult r; r.ok=true; r.handle=e; r.trampoline=tramp;
    impl_->entries.emplace_back(e);
    return r;
#else
    (void)va;
    return MakeError("SafetyHook not available at build time (need deps/SafetyHook amalgamated header)");
#endif
}


bool LinuxHookBackend::ReadPristine(uint64_t rva, uint32_t size, std::vector<uint8_t>& out) const {
    out.clear();
    if (image_base_ == nullptr || size == 0)
        return false;
#if defined(__linux__) && defined(__x86_64__)
    Dl_info di{};
    void* probe = reinterpret_cast<uint8_t*>(image_base_) + rva;
    if (!dladdr(probe, &di) || !di.dli_fname)
        return false;
    FILE* f = fopen(di.dli_fname, "rb");
    if (!f)
        return false;
    Elf64_Ehdr eh{};
    bool ok = fread(&eh, 1, sizeof(eh), f) == sizeof(eh) &&
              eh.e_ident[0] == 0x7f && eh.e_ident[1] == 'E' &&
              eh.e_ident[2] == 'L' && eh.e_ident[3] == 'F' &&
              eh.e_ident[EI_CLASS] == ELFCLASS64;
    // The manifest RVA is relative to the load base; PT_LOAD p_vaddr values
    // are relative to the same base for ET_DYN, so match rva against p_vaddr
    // directly and require the whole range inside one segment's file bytes.
    uint64_t file_off = 0;
    if (ok) {
        ok = false;
        for (int i = 0; i < eh.e_phnum; ++i) {
            Elf64_Phdr ph{};
            if (fseek(f, static_cast<long>(eh.e_phoff + i * sizeof(ph)), SEEK_SET) != 0)
                break;
            if (fread(&ph, 1, sizeof(ph), f) != sizeof(ph))
                break;
            if (ph.p_type != PT_LOAD)
                continue;
            if (rva >= ph.p_vaddr && size <= ph.p_filesz &&
                rva - ph.p_vaddr <= ph.p_filesz - size) {
                file_off = ph.p_offset + (rva - ph.p_vaddr);
                ok = true;
                break;
            }
        }
    }
    if (ok)
        ok = fseek(f, static_cast<long>(file_off), SEEK_SET) == 0;
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
bool LinuxHookBackend::FindReturnOffsets(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (!IsSupported() || size == 0)
        return false;
    // Pristine bytes from the image file, not live memory.
    std::vector<uint8_t> bytes;

    if (!ReadPristine(rva, size, bytes))
        return false;
    return ScanReturnOffsets(bytes.data(), bytes.size(), out);
}
bool LinuxHookBackend::FindInstructionBoundaries(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (!IsSupported() || size == 0)
        return false;
    std::vector<uint8_t> bytes;
    if (!ReadPristine(rva, size, bytes))
        return false;
    return ScanInstructionBoundaries(bytes.data(), bytes.size(), out);
}
InstallResult LinuxHookBackend::InstallTrap(const TargetInfo& target, uint32_t offset_within_symbol) {
    if (!IsSupported())
        return MakeError("backend not supported");
    if (target.point != HookPoint::ENTRY && target.point != HookPoint::OFFSET &&
        target.point != HookPoint::INVOKE && target.point != HookPoint::RETURN)
        return MakeError("traps serve in-function points (ENTRY/OFFSET/INVOKE/RETURN) only");
    if (target.size != 0 && offset_within_symbol >= target.size)
        return MakeError("offset out of range");
#if defined(__linux__) && defined(__x86_64__)
    EnsureTrapSigLinux();
    void* base = target.absolute ? target.absolute : RvaToVa(target.rva);
    if (!base)
        return MakeError("null base");
    auto* va = reinterpret_cast<uint8_t*>(base) + offset_within_symbol;
    uint64_t addr = reinterpret_cast<uint64_t>(va);
    {
        std::lock_guard<std::mutex> g(g_trap_alloc_mutex_linux);
        size_t n = g_trap_next_linux.load(std::memory_order_acquire);
        for (size_t i = 0; i < n && i < kMaxTrapSitesLinux; ++i) {
            if (g_trap_sites_linux[i].addr.load(std::memory_order_acquire) == addr) {
                g_trap_sites_linux[i].refcount.fetch_add(1, std::memory_order_relaxed);
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
        if (n >= kMaxTrapSitesLinux)
            return MakeError("too many trap sites (max 256)");
        uint8_t orig = 0;
        if (!HookMemory::Read(va, &orig, 1))
            return MakeError("cannot read trap site");
        if (orig == kInt3Linux)
            return MakeError("site already holds a breakpoint (foreign int3)");
        {
            HookMemory::ScopedProtect prot(va, 1, MemProt::ReadWriteExecute);
            if (!prot.ok())
                return MakeError("mprotect failed for trap site");
            if (!HookMemory::Write(va, &kInt3Linux, 1))
                return MakeError("cannot write trap site");
            HookMemory::FlushICache(va, 1);
        }
        g_trap_sites_linux[n].orig = orig;
        g_trap_sites_linux[n].target = target;
        g_trap_sites_linux[n].refcount.store(1, std::memory_order_relaxed);
        g_trap_sites_linux[n].addr.store(addr, std::memory_order_release);
        g_trap_next_linux.store(n + 1, std::memory_order_release);
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
InstallResult LinuxHookBackend::InstallVTable(const TargetInfo& target, void* detour){
    if(!IsSupported()) return MakeError("backend not supported");
    if(!detour) return MakeError("null detour");
    void* vt = target.absolute;
    if(!vt) vt = RvaToVa(target.rva);
    if(!vt) return MakeError("null vtable");
    uint32_t slot = target.vtable_slot;
    bool perInst = target.vtable_per_instance;
    void* inst = target.vtable_instance;
#if defined(__linux__)
    if(!perInst){
        if (target.size != 0 && (slot + 1) * sizeof(void*) > target.size) return MakeError("vtable slot out of bounds (manifest)");
        void** slot_ptr = reinterpret_cast<void**>(reinterpret_cast<uint8_t*>(vt) + slot * sizeof(void*));
        HookMemory::ScopedProtect prot(slot_ptr, sizeof(void*), MemProt::ReadWrite);
        if(!prot.ok()) return MakeError("mprotect failed for vtable");
        void* orig = *slot_ptr;
        *slot_ptr = detour;
        HookMemory::FlushICache(slot_ptr, sizeof(void*));
#if HOOK_HAS_SAFETYHOOK_LINUX
        auto* e = new Impl::Entry();
        e->kind = Impl::Entry::Kind::VTable;
        e->vtable_slot_ptr = slot_ptr;
        e->original_vtable_value = orig;
        e->handle = e;
        impl_->entries.emplace_back(e);
        InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig;
        return r;
#else
        InstallResult r; r.ok=true; r.handle=slot_ptr; r.trampoline=orig;
        return r;
#endif
    } else {
        if(!inst) return MakeError("per-instance vtable requires instance pointer");
        if (target.size == 0) return MakeError("vtable size unknown (manifest missing)");
        if ((slot + 1) * sizeof(void*) > target.size) return MakeError("vtable slot out of bounds (manifest)");
        void** obj = reinterpret_cast<void**>(inst);
        void* old_vt = *obj;
        if(!old_vt) return MakeError("instance vtable null");
        size_t bytes = target.size;
        void* new_vt = mmap(nullptr, bytes, PROT_READ|PROT_WRITE, MAP_PRIVATE|MAP_ANONYMOUS, -1, 0);
        if(new_vt==MAP_FAILED) return MakeError("mmap for vtable clone failed");
        memcpy(new_vt, old_vt, bytes);
        void** new_slots = reinterpret_cast<void**>(new_vt);
        void* orig = new_slots[slot];
        new_slots[slot] = detour;
        if(mprotect(new_vt, bytes, PROT_READ|PROT_WRITE)!=0){ munmap(new_vt, bytes); return MakeError("mprotect new vtable failed"); }
        HookMemory::FlushICache(new_vt, bytes);
        HookMemory::ScopedProtect iprot(obj, sizeof(void*), MemProt::ReadWrite);
        if(!iprot.ok()){ munmap(new_vt, bytes); return MakeError("mprotect instance failed"); }
        *obj = new_vt;
        HookMemory::FlushICache(obj, sizeof(void*));
#if HOOK_HAS_SAFETYHOOK_LINUX
        auto* e = new Impl::Entry();
        e->kind = Impl::Entry::Kind::VTable;
        e->vtable_slot_ptr = &new_slots[slot];
        e->original_vtable_value = orig;
        e->cloned_vtable = new_vt;
        e->cloned_count = bytes / sizeof(void*);
        e->instance_ptr = inst;
        e->original_instance_vtable = old_vt;
        e->handle = e;
        impl_->entries.emplace_back(e);
        InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig;
        return r;
#else
        InstallResult r; r.ok=true; r.handle=obj; r.trampoline=orig;
        return r;
#endif
    }
#else
    (void)slot; (void)perInst; (void)inst; (void)vt;
    return MakeError("vtable not supported on this platform");
#endif
}

InstallResult LinuxHookBackend::InstallGot(const TargetInfo& target, void* detour){
    if(!IsSupported()) return MakeError("backend not supported");
    if(!detour) return MakeError("null detour");
    std::string mod = target.import_module;
    std::string sym = target.import_symbol;
    if(sym.empty()) return MakeError("GOT requires import_symbol");
    std::string err;
    void* slot = FindGotSlot(mod, sym, err);
    if(!slot) return MakeError(err);
    HookMemory::ScopedProtect prot(slot, sizeof(void*), MemProt::ReadWrite);
    if(!prot.ok()) return MakeError("mprotect failed for GOT slot");
    void* orig = *reinterpret_cast<void**>(slot);
    *reinterpret_cast<void**>(slot) = detour;
    HookMemory::FlushICache(slot, sizeof(void*));
#if HOOK_HAS_SAFETYHOOK_LINUX
    auto* e = new Impl::Entry();
    e->kind = Impl::Entry::Kind::Got;
    e->got_slot_ptr = slot;
    e->original_got_value = orig;
    e->handle = e;
    impl_->entries.emplace_back(e);
    InstallResult r; r.ok=true; r.handle=e; r.trampoline=orig;
    return r;
#else
    InstallResult r; r.ok=true; r.handle=slot; r.trampoline=orig;
    return r;
#endif
}

InstallResult LinuxHookBackend::InstallIAT(const TargetInfo& target, void* detour){
    return InstallGot(target, detour);
}

bool LinuxHookBackend::Remove(HookHandle handle, std::string& error){
    if(!handle){ error="null handle"; return false; }
#if HOOK_HAS_SAFETYHOOK_LINUX
    for(auto it=impl_->entries.begin(); it!=impl_->entries.end(); ++it){
        if((*it)->handle==handle || it->get()==handle){
            auto* e=it->get();
            bool ok=true;
            switch(e->kind){
                case Impl::Entry::Kind::Inline: e->inlineHook.reset(); break;
                case Impl::Entry::Kind::Trap: {
#if defined(__linux__) && defined(__x86_64__)
                    size_t slot = e->trap_slot;
                    if (slot < kMaxTrapSitesLinux) {
                        int left = g_trap_sites_linux[slot].refcount.fetch_sub(1, std::memory_order_acq_rel) - 1;
                        if (left <= 0) {
                            void* va = reinterpret_cast<void*>(g_trap_sites_linux[slot].addr.load(std::memory_order_acquire));
                            uint8_t orig = g_trap_sites_linux[slot].orig;
                            if (va) {
                                HookMemory::ScopedProtect p(va, 1, MemProt::ReadWriteExecute);
                                if (p.ok()) {
                                    HookMemory::Write(va, &orig, 1);
                                    HookMemory::FlushICache(va, 1);
                                } else ok = false;
                            }
                            g_trap_sites_linux[slot].addr.store(0, std::memory_order_release);
                        }
                    }
#endif
                    break;
                }
                case Impl::Entry::Kind::VTable: {
                    if(e->cloned_vtable){
                        HookMemory::ScopedProtect p(e->instance_ptr? reinterpret_cast<void**>(e->instance_ptr): e->vtable_slot_ptr, sizeof(void*), MemProt::ReadWrite);
                        if(e->instance_ptr) *reinterpret_cast<void**>(e->instance_ptr)=e->original_instance_vtable;
                        else if(e->vtable_slot_ptr) *reinterpret_cast<void**>(e->vtable_slot_ptr)=e->original_vtable_value;
                        munmap(e->cloned_vtable, e->cloned_count * sizeof(void*));
                    } else if(e->vtable_slot_ptr){
                        HookMemory::ScopedProtect p(e->vtable_slot_ptr, sizeof(void*), MemProt::ReadWrite);
                        if(p.ok()) *reinterpret_cast<void**>(e->vtable_slot_ptr)=e->original_vtable_value;
                        else ok=false;
                    }
                    break;
                }
                case Impl::Entry::Kind::Got: {
                    HookMemory::ScopedProtect p(e->got_slot_ptr, sizeof(void*), MemProt::ReadWrite);
                    if(p.ok()) *reinterpret_cast<void**>(e->got_slot_ptr)=e->original_got_value;
                    else ok=false;
                    break;
                }
            }
            impl_->entries.erase(it);
            if(!ok){ error="mprotect failed during remove"; return false; }
            return true;
        }
    }
    error="handle not found";
    return false;
#else
    (void)handle; error="SafetyHook not available";
    return false;
#endif
}

} // namespace Slic3r::Hook
