#pragma once

#include "../CpuContext.hpp"
#include <cstdint>

namespace Slic3r::Hook::Arch::X64 {

// Stubs build a CpuContext on the stack and call the C++ dispatcher, which
// runs the chain without holding registry locks. Both ABIs share the
// CpuContext layout; Windows uses Microsoft x64 (rcx/rdx/r8, 32-byte shadow),
// Linux uses SysV (rdi/rsi/rdx, PLT call). HookMemory flushes the icache
// after any code patch; stubs themselves are never patched at runtime.

extern "C" {

// Implemented in C++; must stay noexcept and allocation-free.

bool orca_hook_inline_dispatch(void* target_info, CpuContext* ctx, void** trampoline) noexcept;
void orca_hook_mid_dispatch(void* target_info, CpuContext* ctx) noexcept;

// Detours installed by the backend for slot swaps; never patched at runtime.
void orca_hook_inline_stub() noexcept;
void orca_hook_mid_stub() noexcept;

// Invokes the original through CpuContext; used when the chain calls through.
void orca_hook_call_trampoline(void* trampoline, CpuContext* ctx) noexcept;

} // extern "C"

// Snapshot helper; allocation-free.
inline void FillCpuContextFromSnapshot(CpuContext& out,
                                      uint64_t rax, uint64_t rcx, uint64_t rdx, uint64_t rbx,
                                      uint64_t rsp, uint64_t rbp, uint64_t rsi, uint64_t rdi,
                                      uint64_t r8, uint64_t r9, uint64_t r10, uint64_t r11,
                                      uint64_t r12, uint64_t r13, uint64_t r14, uint64_t r15,
                                      uint64_t rip, uint64_t rflags,
                                      const XmmReg* xmm) noexcept {
    out.size = sizeof(CpuContext);
    out.rax = rax; out.rcx = rcx; out.rdx = rdx; out.rbx = rbx;
    out.rsp = rsp; out.rbp = rbp; out.rsi = rsi; out.rdi = rdi;
    out.r8 = r8; out.r9 = r9; out.r10 = r10; out.r11 = r11;
    out.r12 = r12; out.r13 = r13; out.r14 = r14; out.r15 = r15;
    out.rip = rip; out.rflags = rflags;
    if (xmm) for (int i=0;i<16;++i) out.xmm[i]=xmm[i];
}

// Rebuild-stability probe: stub bytes must hash identically for identical inputs.
inline constexpr uint32_t kStubsVersion = 1;
} // namespace Slic3r::Hook::Arch::X64
