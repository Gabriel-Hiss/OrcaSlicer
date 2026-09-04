#pragma once
#include "hook_target.hpp"
#include "detail/registry.hpp"
#include "detail/cpu_context.hpp"
#include <cstdint>

#if defined(__clang__) || defined(__GNUC__)
#define ORCA_KEEP_ATTR __attribute__((used, retain))
#else
#define ORCA_KEEP_ATTR
#endif

namespace orca::plugin {
inline constexpr int kDefaultPriority = 1000;

#define ORCA_BEFORE(target, func) ORCA_BEFORE_PRIO(target, func, ::orca::plugin::kDefaultPriority)
#define ORCA_AFTER(target, func) ORCA_AFTER_PRIO(target, func, ::orca::plugin::kDefaultPriority)
#define ORCA_REPLACE(target, func) ORCA_REPLACE_PRIO(target, func, ::orca::plugin::kDefaultPriority)

#define ORCA_BEFORE_PRIO(target, func, prio) \
    void func() noexcept; \
    extern "C" orca_hook_status_t func##_orca_before_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept { \
        (void)ctx; (void)user_data; \
        try { func(); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
        if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } \
        return ORCA_HOOK_OK; \
    } \
    extern "C" int func##_orca_before_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_before_keep")) \
    extern "C" int func##_orca_before_keep = ([]{ ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=(target).symbol_id; r.rva=(target).rva; r.point=::orca::plugin::HookPoint::Entry; r.kind=::orca::plugin::HookKind::Before; r.priority=(prio); r.trampoline=(void*)func##_orca_before_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_AFTER_PRIO(target, func, prio) \
    void func() noexcept; \
    extern "C" orca_hook_status_t func##_orca_after_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept { \
        (void)ctx; (void)user_data; \
        try { func(); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
        if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } \
        return ORCA_HOOK_OK; \
    } \
    extern "C" int func##_orca_after_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_after_keep")) \
    extern "C" int func##_orca_after_keep = ([]{ ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=(target).symbol_id; r.rva=(target).rva; r.point=::orca::plugin::HookPoint::Entry; r.kind=::orca::plugin::HookKind::After; r.priority=(prio); r.trampoline=(void*)func##_orca_after_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_REPLACE_PRIO(target, func, prio) \
    orca_hook_status_t func(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept, void* user_data) noexcept; \
    extern "C" orca_hook_status_t func##_orca_replace_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept, void* user_data) noexcept { \
        try { return func(ctx,out_result,next,user_data); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
    } \
    extern "C" int func##_orca_replace_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_replace_keep")) \
    extern "C" int func##_orca_replace_keep = ([]{ ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=(target).symbol_id; r.rva=(target).rva; r.point=::orca::plugin::HookPoint::Entry; r.kind=::orca::plugin::HookKind::Replace; r.priority=(prio); r.trampoline=(void*)func##_orca_replace_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_MID(target, off_rva, func) \
    orca_hook_status_t func(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept; \
    extern "C" orca_hook_status_t func##_orca_mid_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept { \
        try { return func(ctx,out_result,user_data); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
    } \
    extern "C" int func##_orca_mid_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_mid_keep")) \
    extern "C" int func##_orca_mid_keep = ([]{ auto t=(target); t.point=::orca::plugin::HookPoint::Offset; t.offset_rva=(off_rva); ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=t.symbol_id; r.rva=t.rva; r.point=t.point; r.kind=::orca::plugin::HookKind::Before; r.priority=::orca::plugin::kDefaultPriority; r.offset_rva=t.offset_rva; r.trampoline=(void*)func##_orca_mid_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_VTABLE(target, slot, func) \
    orca_hook_status_t func(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept; \
    extern "C" orca_hook_status_t func##_orca_vtable_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept { \
        try { return func(ctx,out_result,user_data); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
    } \
    extern "C" int func##_orca_vtable_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_vtable_keep")) \
    extern "C" int func##_orca_vtable_keep = ([]{ auto t=(target); t.point=::orca::plugin::HookPoint::VTable; t.vtable_index=(slot); ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=t.symbol_id; r.rva=t.rva; r.point=t.point; r.kind=::orca::plugin::HookKind::Replace; r.priority=::orca::plugin::kDefaultPriority; r.vtable_index=t.vtable_index; r.trampoline=(void*)func##_orca_vtable_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_IMPORT(target, mod, name, func) \
    orca_hook_status_t func(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept; \
    extern "C" orca_hook_status_t func##_orca_import_trampoline(orca_cpu_context_t* ctx, orca_hook_result_t* out_result, void* user_data) noexcept { \
        try { return func(ctx,out_result,user_data); } catch(...) { if(out_result){ out_result->size=sizeof(*out_result); out_result->version=ORCA_HOOK_ABI_VERSION; out_result->action=ORCA_HOOK_ACTION_CONTINUE; } return ORCA_HOOK_ERR_INTERNAL; } \
    } \
    extern "C" int func##_orca_import_keep ORCA_KEEP_ATTR; \
    __pragma(comment(linker, "/include:" #func "_orca_import_keep")) \
    extern "C" int func##_orca_import_keep = ([]{ auto t=(target); t.point=::orca::plugin::HookPoint::Iat; t.import_module=(mod); t.import_name=(name); ::orca::plugin::detail::HookRegistration r{}; r.hook_id=#func; r.symbol_id=t.symbol_id; r.rva=t.rva; r.point=t.point; r.kind=::orca::plugin::HookKind::Replace; r.priority=::orca::plugin::kDefaultPriority; r.import_module=t.import_module; r.import_name=t.import_name; r.trampoline=(void*)func##_orca_import_trampoline; ::orca::plugin::detail::register_hook(r); return 0; }(), 0);

#define ORCA_IMPORT_HOOK ORCA_IMPORT

struct HookPriority { int value; };
inline HookPriority priority(int p) { return {p}; }

}
