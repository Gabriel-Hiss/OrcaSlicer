#pragma once
#include "../hook_target.hpp"
#include <vector>
namespace orca::plugin::detail {
struct HookRegistration {
    const char* hook_id = nullptr;
    const char* symbol_id = nullptr;
    uint32_t rva = 0;
    HookPoint point = HookPoint::Entry;
    HookKind kind = HookKind::Before;
    int priority = 1000;
    void* trampoline = nullptr;
    void* user_data = nullptr;
    uint32_t ordinal = 0;
    uint32_t offset_rva = 0;
    uint32_t vtable_index = 0;
    const char* import_module = nullptr;
    const char* import_name = nullptr;
};
void register_hook(const HookRegistration& reg) noexcept;
std::vector<HookRegistration> take_registrations() noexcept;
}
