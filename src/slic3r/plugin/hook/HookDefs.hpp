#pragma once

#include <cstdint>
#include <string>

namespace Slic3r::Hook {

// Stable ABI version.
inline constexpr uint32_t kHookAbiVersion = 1;
inline constexpr uint32_t kManifestFormatVersion = 1;

// Larger runs first.
inline constexpr int32_t kDefaultPriority = 1000;

enum class HookPoint : uint8_t {
    ENTRY  = 0,  // function entry (breakpoint-trap at prologue)
    RETURN = 1,  // function return(s), one breakpoint-trap per RET site
    INVOKE = 2,  // call site inside target (requires ordinal)
    OFFSET = 3,  // arbitrary RVA interior at validated instruction boundary
    VTABLE = 4,  // virtual slot (index or global slot patch / per-instance clone)
    IAT    = 5,  // import address table (Windows) / GOT (Linux aliased)
    GOT    = 6,  // Linux GOT/PLT (treated as IAT in portable SDK, distinct in backend)
    UNKNOWN = 255
};

enum class HookKind : uint8_t {
    Before  = 0, // may mutate args via CpuContext, may cancel (skip replace/original)
    After   = 1, // may mutate return value; runs even after cancellation
    Replace = 2, // receives next(); may call at most once; wraps original
    Raw     = 3, // raw trap at instruction boundary, full CpuContext access
    UNKNOWN = 255
};

enum class HookStatus : int32_t {
    Ok                  = 0,
    InvalidArg          = 1,
    NotFound            = 2,
    AlreadyInstalled    = 3,
    BuildMismatch       = 4,
    BadInstructionBoundary = 5,
    ProtectionFailed    = 6,
    SafetyHookFailed    = 7,
    Timeout             = 8,
    RestartRequired     = 9,
    Disabled            = 10,
    InternalError       = 11
};

inline const char* to_string(HookPoint p) noexcept {
    switch (p) {
        case HookPoint::ENTRY:  return "ENTRY";
        case HookPoint::RETURN: return "RETURN";
        case HookPoint::INVOKE: return "INVOKE";
        case HookPoint::OFFSET: return "OFFSET";
        case HookPoint::VTABLE: return "VTABLE";
        case HookPoint::IAT:    return "IAT";
        case HookPoint::GOT:    return "GOT";
        default:                return "UNKNOWN";
    }
}

inline const char* to_string(HookKind k) noexcept {
    switch (k) {
        case HookKind::Before:  return "before";
        case HookKind::After:   return "after";
        case HookKind::Replace: return "replace";
        case HookKind::Raw:     return "raw";
        default:                return "unknown";
    }
}

// Drain semantics: disable/reload removes from chain then waits.
// If active callbacks exceed timeout, module is left resident.
inline constexpr uint32_t kDefaultDrainTimeoutMs = 3000;

// Opaque handle exposed to plugins (mirrors orca_hook_handle_t).
using HookHandle = void*;

using HookId = std::string;
using PluginId = std::string;
using TargetId = std::string;

struct InvokeDetail {
    uint32_t ordinal = 0;
};

struct OffsetDetail {
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct VTableDetail {
    uint32_t slot = 0;
    bool per_instance = false;
    void* instance = nullptr;
};

struct ImportDetail {
    std::string module;
    std::string symbol;
};

} // namespace Slic3r::Hook
