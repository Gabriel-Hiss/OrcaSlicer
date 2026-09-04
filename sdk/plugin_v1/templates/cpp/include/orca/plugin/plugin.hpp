#pragma once
#include "hook.hpp"
#include "raw.hpp"
#include <cstdint>
#include "detail/cpu_context.hpp" // brings orca_hook_api.h for orca_host_api_v1_t and orca_hook_status_t

namespace orca::plugin {
extern "C" {
    int on_plugin_load(const orca_host_api_v1_t* host) noexcept;
    int on_plugin_unload(const orca_host_api_v1_t* host) noexcept;
}
#define ORCA_PLUGIN_ON_LOAD(fn) \
    extern "C" int on_plugin_load(const orca_host_api_v1_t* host) noexcept { return fn(host); }
#define ORCA_PLUGIN_ON_UNLOAD(fn) \
    extern "C" int on_plugin_unload(const orca_host_api_v1_t* host) noexcept { return fn(host); }
} // namespace orca::plugin

// Exports are declared in orca_hook_api.h (ORCA_HOOK_API). Do not redeclare here to avoid attribute mismatch.
// The actual definitions are in src/plugin_entry.cpp which includes orca_hook_api.h directly.
#if 0
extern "C" {
#if defined(_WIN32)
#define ORCA_HOOK_EXPORT __declspec(dllexport)
#else
#define ORCA_HOOK_EXPORT __attribute__((visibility("default")))
#endif
ORCA_HOOK_EXPORT orca_hook_status_t orca_plugin_entry_v1(const orca_host_api_v1_t* host) noexcept;
ORCA_HOOK_EXPORT orca_hook_status_t orca_plugin_exit_v1(const orca_host_api_v1_t* host) noexcept;
}
#endif
// Provide ORCA_HOOK_EXPORT for plugin_entry.cpp
#if defined(_WIN32)
#define ORCA_HOOK_EXPORT __declspec(dllexport)
#else
#define ORCA_HOOK_EXPORT __attribute__((visibility("default")))
#endif
