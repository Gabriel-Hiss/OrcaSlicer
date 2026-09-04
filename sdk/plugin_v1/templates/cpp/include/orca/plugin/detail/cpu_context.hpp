#pragma once
#include <cstdint>
#include <cstddef>
#if __has_include("orca_hook_api.h")
#include "orca_hook_api.h"
#elif __has_include("abi/orca_hook_api.h")
#include "abi/orca_hook_api.h"
#elif __has_include("../../../abi/orca_hook_api.h")
#include "../../../abi/orca_hook_api.h"
#else
#error "canonical sdk/plugin_v1/abi/orca_hook_api.h not found — generated SDK must copy it to abi/orca_hook_api.h"
#endif
namespace orca::plugin::detail {
class CpuContext {
public:
    explicit CpuContext(orca_cpu_context_t* ctx) noexcept : ctx_(ctx) {}
    orca_cpu_context_t* raw() noexcept { return ctx_; }
    const orca_cpu_context_t* raw() const noexcept { return ctx_; }
    uint64_t& rax() noexcept { return ctx_->rax; }
    uint64_t& rbx() noexcept { return ctx_->rbx; }
    uint64_t& rcx() noexcept { return ctx_->rcx; }
    uint64_t& rdx() noexcept { return ctx_->rdx; }
    uint64_t& rsi() noexcept { return ctx_->rsi; }
    uint64_t& rdi() noexcept { return ctx_->rdi; }
    uint64_t& rbp() noexcept { return ctx_->rbp; }
    uint64_t& rsp() noexcept { return ctx_->rsp; }
    uint64_t& r8()  noexcept { return ctx_->r8; }
    uint64_t& r9()  noexcept { return ctx_->r9; }
    uint64_t& r10() noexcept { return ctx_->r10; }
    uint64_t& r11() noexcept { return ctx_->r11; }
    uint64_t& r12() noexcept { return ctx_->r12; }
    uint64_t& r13() noexcept { return ctx_->r13; }
    uint64_t& r14() noexcept { return ctx_->r14; }
    uint64_t& r15() noexcept { return ctx_->r15; }
    uint64_t& rip() noexcept { return ctx_->rip; }
    uint64_t& flags() noexcept { return ctx_->rflags; }
    bool read_memory(uint64_t addr, void* out, size_t len) const noexcept;
    bool write_memory(uint64_t addr, const void* data, size_t len) noexcept;
private:
    orca_cpu_context_t* ctx_;
};
}
