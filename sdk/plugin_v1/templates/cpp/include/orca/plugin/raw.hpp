#pragma once
#include "detail/cpu_context.hpp"
#include <cstdint>
#include <cstddef>
#include <type_traits>

namespace orca::plugin {

using detail::CpuContext;

/// Borrowed handle for C++ aggregates that are not trivially-copyable / STL.
/// Never owns, never destructs. Explicitly non-copyable ownership semantics
/// are enforced by the type system — you must not delete or store beyond
/// the hook callback lifetime.
template <typename T>
class Borrowed {
public:
    explicit Borrowed(T* ptr) noexcept : ptr_(ptr) {}
    Borrowed(const Borrowed&) = default;
    Borrowed& operator=(const Borrowed&) = default;

    T* get() const noexcept { return ptr_; }
    T* operator->() const noexcept { return ptr_; }
    T& operator*() const noexcept { return *ptr_; }
    explicit operator bool() const noexcept { return ptr_ != nullptr; }
    T* release() const noexcept { return ptr_; } // does not transfer ownership

private:
    T* ptr_;
};

/// RawHook escape — gives direct access to CpuContext, host table, and
/// unsafe memory operations. All operations are explicit and unchecked
/// against the type table; misuse is UB and must be validated against
/// the manifest typed_binding.reason when available.
class RawHook {
public:
    explicit RawHook(orca_cpu_context_t* ctx, const orca_host_api_v1* host) noexcept
        : ctx_(ctx), host_(host) {}

    CpuContext context() noexcept { return CpuContext(ctx_); }
    const orca_host_api_v1* host() const noexcept { return host_; }

    // Explicit sized memory access. Returns false on host failure.
    bool read(uint64_t addr, void* out, size_t len) const noexcept;
    bool write(uint64_t addr, const void* data, size_t len) const noexcept;
    bool protect(uint64_t addr, size_t len, uint32_t prot) const noexcept;
    void flush_icache(uint64_t addr, size_t len) const noexcept;

    // Typed helpers — only for trivially copyable types. Caller guarantees
    // address validity and lifetime (borrowed, not owned).
    template <typename T>
    std::enable_if_t<std::is_trivially_copyable_v<T>, bool>
    read_value(uint64_t addr, T& out) const noexcept {
        return read(addr, &out, sizeof(T));
    }
    template <typename T>
    std::enable_if_t<std::is_trivially_copyable_v<T>, bool>
    write_value(uint64_t addr, const T& v) const noexcept {
        return write(addr, &v, sizeof(T));
    }

private:
    orca_cpu_context_t* ctx_;
    const orca_host_api_v1* host_;
};

} // namespace orca::plugin
