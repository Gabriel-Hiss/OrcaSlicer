#include "HookMemory.hpp"

#include <cstring>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#include <windows.h>
#else
#include <sys/mman.h>
#include <unistd.h>
#endif

namespace Slic3r::Hook {

bool HookMemory::Read(const void* src, void* dst, size_t size) noexcept {
    if (!src || !dst || size == 0) return false;
    // Probe with VirtualQuery; a fault stays fatal and is annotated thread-locally.
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(src, &mbi, sizeof(mbi)) == 0) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    if ((mbi.State & MEM_COMMIT) == 0) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t end  = base + mbi.RegionSize;
    uintptr_t s = reinterpret_cast<uintptr_t>(src);
    if (s < base || s + size > end) return false;
#endif
    // nosan: memcpy is safe after probe
    std::memcpy(dst, src, size);
    return true;
}

bool HookMemory::Write(void* dst, const void* src, size_t size) noexcept {
    if (!dst || !src || size == 0) return false;
#ifdef _WIN32
    MEMORY_BASIC_INFORMATION mbi{};
    if (VirtualQuery(dst, &mbi, sizeof(mbi)) == 0) return false;
    if ((mbi.Protect & (PAGE_GUARD | PAGE_NOACCESS)) != 0) return false;
    if ((mbi.State & MEM_COMMIT) == 0) return false;
    uintptr_t base = reinterpret_cast<uintptr_t>(mbi.BaseAddress);
    uintptr_t end  = base + mbi.RegionSize;
    uintptr_t d = reinterpret_cast<uintptr_t>(dst);
    if (d < base || d + size > end) return false;
    // Need writable. Caller is expected to Protect(RW) around patch.
    // Do not change prot here — just probe.
#endif
    std::memcpy(dst, src, size);
    return true;
}

uint32_t HookMemory::ToOsProt(MemProt p) noexcept {
#ifdef _WIN32
    bool r = (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Read)) != 0;
    bool w = (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Write)) != 0;
    bool x = (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Execute)) != 0;
    if (r && w && x) return PAGE_EXECUTE_READWRITE;
    if (r && x)      return PAGE_EXECUTE_READ;
    if (r && w)      return PAGE_READWRITE;
    if (r)           return PAGE_READONLY;
    if (x)           return PAGE_EXECUTE;
    return PAGE_NOACCESS;
#else
    int prot = 0;
    if (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Read))    prot |= PROT_READ;
    if (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Write))   prot |= PROT_WRITE;
    if (static_cast<uint32_t>(p) & static_cast<uint32_t>(MemProt::Execute)) prot |= PROT_EXEC;
    return static_cast<uint32_t>(prot);
#endif
}

MemProt HookMemory::FromOsProt(uint32_t os) noexcept {
#ifdef _WIN32
    switch (os & 0xFF) {
        case PAGE_EXECUTE_READWRITE: return MemProt::ReadWriteExecute;
        case PAGE_EXECUTE_READ:      return MemProt::ReadExecute;
        case PAGE_EXECUTE_WRITECOPY: return MemProt::ReadWriteExecute;
        case PAGE_READWRITE:         return MemProt::ReadWrite;
        case PAGE_WRITECOPY:         return MemProt::ReadWrite;
        case PAGE_READONLY:          return MemProt::Read;
        case PAGE_EXECUTE:           return MemProt::Execute;
        default:                     return MemProt::Read;
    }
#else
    MemProt p = static_cast<MemProt>(0);
    if (os & PROT_READ)  p = static_cast<MemProt>(static_cast<uint32_t>(p) | static_cast<uint32_t>(MemProt::Read));
    if (os & PROT_WRITE) p = static_cast<MemProt>(static_cast<uint32_t>(p) | static_cast<uint32_t>(MemProt::Write));
    if (os & PROT_EXEC)  p = static_cast<MemProt>(static_cast<uint32_t>(p) | static_cast<uint32_t>(MemProt::Execute));
    return p;
#endif
}

bool HookMemory::Protect(void* addr, size_t size, MemProt new_prot, MemProt* out_old) noexcept {
    if (!addr || size == 0) return false;
#ifdef _WIN32
    DWORD old = 0;
    DWORD np = ToOsProt(new_prot);
    BOOL ok = VirtualProtect(addr, size, np, &old);
    if (!ok) return false;
    if (out_old) *out_old = FromOsProt(old);
    return true;
#else
    // Align to page boundary for mprotect
    long pagesz = sysconf(_SC_PAGESIZE);
    if (pagesz <= 0) pagesz = 4096;
    uintptr_t a = reinterpret_cast<uintptr_t>(addr);
    uintptr_t a0 = a & ~(static_cast<uintptr_t>(pagesz - 1));
    uintptr_t a1 = (a + size + pagesz - 1) & ~(static_cast<uintptr_t>(pagesz - 1));
    size_t len = a1 - a0;
    // Query old - not easily available; report ReadWriteExecute conservatively
    // Caller uses RAII to restore to previous, so we stash RWX.
    if (out_old) *out_old = MemProt::ReadWriteExecute;
    int np = static_cast<int>(ToOsProt(new_prot));
    return mprotect(reinterpret_cast<void*>(a0), len, np) == 0;
#endif
}

void HookMemory::FlushICache(void* addr, size_t size) noexcept {
    if (!addr || size == 0) return;
#ifdef _WIN32
    FlushInstructionCache(GetCurrentProcess(), addr, size);
#else
#if defined(__GNUC__) || defined(__clang__)
    __builtin___clear_cache(reinterpret_cast<char*>(addr), reinterpret_cast<char*>(addr) + size);
#else
    (void)addr; (void)size;
#endif
#endif
}

} // namespace Slic3r::Hook
