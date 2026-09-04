#pragma once

#include <cstdint>
#include <cstring>
#include "../../../../sdk/plugin_v1/abi/orca_hook_api.h"

namespace Slic3r::Hook {

using CpuContext = orca_cpu_context_t;
using XmmReg = orca_xmm_t;
using HookResult = orca_hook_result_t;
using HookAction = orca_hook_action_t;
using HookStatusAbi = orca_hook_status_t;

inline HookResult MakeContinueResult() noexcept {
    HookResult r{};
    r.size = static_cast<uint32_t>(sizeof(HookResult));
    r.version = ORCA_HOOK_ABI_VERSION;
    r.action = ORCA_HOOK_ACTION_CONTINUE;
    return r;
}
inline bool IsSkipOriginal(const HookResult& r) noexcept { return r.action == ORCA_HOOK_ACTION_SKIP_ORIGINAL; }

// Active hook annotated per thread so a fatal access violation can name it.
struct ActiveHookScope {
    const char* plugin_id = nullptr;
    const char* hook_id = nullptr;
    const char* target_id = nullptr;
};
inline thread_local ActiveHookScope t_active_hook{};

// A hook reentered on its own thread skips only its own record.
inline thread_local void* t_reentry_stack[32] = {nullptr};
inline thread_local uint32_t t_reentry_depth = 0;

inline bool is_reentering(void* hook_handle) noexcept {
    for (uint32_t i = 0; i < t_reentry_depth; ++i)
        if (t_reentry_stack[i] == hook_handle) return true;
    return false;
}
inline void push_reentry(void* hook_handle) noexcept {
    if (t_reentry_depth < 32) t_reentry_stack[t_reentry_depth++] = hook_handle;
}
inline void pop_reentry() noexcept {
    if (t_reentry_depth > 0) --t_reentry_depth;
}

} // namespace Slic3r::Hook
