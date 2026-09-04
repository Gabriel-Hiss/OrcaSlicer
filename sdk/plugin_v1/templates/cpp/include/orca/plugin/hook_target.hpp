#pragma once
#include "raw.hpp"
#include <cstdint>
#include <type_traits>
#include <utility>

namespace orca::plugin {

enum class HookPoint : uint32_t {
    Entry  = 0, // function entry (inline hook)
    Return = 1, // return site
    Invoke = 2, // specific call site inside function
    Offset = 3, // arbitrary RVA within function range, instruction-boundary validated
    VTable = 4, // vtable slot (global or per-instance)
    Iat    = 5, // Windows IAT
    Got    = 6, // Linux GOT/PLT — alias of Iat via import_hook
};

enum class HookKind : uint32_t {
    Before  = 0,
    After   = 1,
    Replace = 2,
};

/// Typed HookTarget — parameterized by the exact Orca symbol signature.
/// Only trivially-copyable, fixed-size, non-varargs signatures get a typed
/// binding; everything else is exposed via RawHook only (see typed_binding
/// in manifest). The generator emits one instantiation per symbol with
/// available==true.
///
/// Example (generated):
///   using PrintHelpTarget = HookTarget<void(*)()>; // Slic3r::CLI::print_help
///   // or member: HookTarget<void(Slic3r::CLI::*)()>

template <typename Sig>
struct HookTarget;

// Free function / static
template <typename Ret, typename... Args>
struct HookTarget<Ret(*)(Args...)> {
    using signature = Ret(*)(Args...);
    using return_type = Ret;
    static constexpr bool is_member = false;
    static constexpr bool typed_available = true; // overridden by generator if raw

    const char* symbol_id; // manifest id, e.g. "Slic3r::CLI::print_help"
    uint32_t rva;
    HookPoint point = HookPoint::Entry;
    // For INVOKE/OFFSET/VTABLE/IAT/GOT, filled by hook declaration:
    uint32_t ordinal = 0;
    uint32_t offset_rva = 0;
    uint32_t vtable_index = 0;
    const char* import_module = nullptr;
    const char* import_name = nullptr;
};

// Member function
template <typename Ret, typename Cls, typename... Args>
struct HookTarget<Ret(Cls::*)(Args...)> {
    using signature = Ret(Cls::*)(Args...);
    using return_type = Ret;
    using class_type = Cls;
    static constexpr bool is_member = true;
    static constexpr bool typed_available = true;

    const char* symbol_id;
    uint32_t rva;
    HookPoint point = HookPoint::Entry;
    uint32_t ordinal = 0;
    uint32_t offset_rva = 0;
    uint32_t vtable_index = 0;
    const char* import_module = nullptr;
    const char* import_name = nullptr;
};

template <typename Ret, typename Cls, typename... Args>
struct HookTarget<Ret(Cls::*)(Args...) const> {
    using signature = Ret(Cls::*)(Args...) const;
    using return_type = Ret;
    using class_type = Cls;
    static constexpr bool is_member = true;
    static constexpr bool typed_available = true;
    const char* symbol_id;
    uint32_t rva;
    HookPoint point = HookPoint::Entry;
    uint32_t ordinal = 0;
    uint32_t offset_rva = 0;
    uint32_t vtable_index = 0;
    const char* import_module = nullptr;
    const char* import_name = nullptr;
};

/// Borrowed handle helpers for non-trivial aggregates (classes, STL).
/// Generator emits using BorrowedHandle = Borrowed<T> for each UDT that
/// crosses the boundary as borrowed.
template <typename T>
using BorrowedHandle = Borrowed<T>;

// Call-next token for replace hooks. Can be invoked at most once per
// activation; second call returns default-constructed Ret and sets error.
template <typename Ret, typename... Args>
class Next {
public:
    using Fn = Ret(*)(Args...);
    explicit Next(Fn f) noexcept : fn_(f), called_(false) {}
    Ret operator()(Args... args) noexcept {
        if (called_ || !fn_) return Ret{};
        called_ = true;
        return fn_(std::forward<Args>(args)...);
    }
    bool called() const noexcept { return called_; }
private:
    Fn fn_;
    bool called_;
};

template <typename... Args>
class Next<void, Args...> {
public:
    using Fn = void(*)(Args...);
    explicit Next(Fn f) noexcept : fn_(f), called_(false) {}
    void operator()(Args... args) noexcept {
        if (called_ || !fn_) return;
        called_ = true;
        fn_(std::forward<Args>(args)...);
    }
    bool called() const noexcept { return called_; }
private:
    Fn fn_;
    bool called_;
};

} // namespace orca::plugin
