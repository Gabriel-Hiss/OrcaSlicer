#include "Dispatch.hpp"
#include "CpuContext.hpp"
#include <exception>
#include <stdexcept>

namespace Slic3r::Hook {

// Adapts the ABI plain-function-pointer next() to the C++ chain.
namespace {
inline thread_local std::function<HookStatusAbi(CpuContext*)> t_abi_next_fn{};
inline HookStatusAbi abi_next_shim(CpuContext* ctx) noexcept {
    if (t_abi_next_fn) {
        try { return t_abi_next_fn(ctx); }
        catch (...) { return static_cast<HookStatusAbi>(99); }
    }
    return static_cast<HookStatusAbi>(0);
}
}

bool Dispatcher::InvokeBefore(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx, bool& cancelled) {
    cancelled = false;
    bool has_before = (hook->before != nullptr) || (hook->abi_callback != nullptr && hook->kind == HookKind::Before);
    if (!has_before) return true;
    if (hook->disabled_this_session) return true;
    if (is_reentering(hook.get())) return true;
    push_reentry(hook.get());
    ActiveHookScope prev = t_active_hook;
    t_active_hook = {hook->plugin_id.c_str(), hook->hook_id.c_str(), hook->target_id.c_str()};
    HookResult out = MakeContinueResult();
    HookStatusAbi status = static_cast<HookStatusAbi>(0);
    bool ok = true;
    try {
        if (hook->before) {
            status = hook->before(ctx, &out);
        } else if (hook->abi_callback) {
            auto fn = reinterpret_cast<orca_hook_callback_t>(hook->abi_callback);
            out.size = static_cast<uint32_t>(sizeof(HookResult));
            out.version = ORCA_HOOK_ABI_VERSION;
            out.action = ORCA_HOOK_ACTION_CONTINUE;
            status = fn(reinterpret_cast<orca_cpu_context_t*>(ctx), reinterpret_cast<orca_hook_result_t*>(&out), hook->abi_user_data);
        }
    } catch (const std::exception& e){
        hook->disabled_this_session = true;
        hook->last_error = std::string("before exception: ")+e.what();
        ok = false;
    } catch (...){
        hook->disabled_this_session = true;
        hook->last_error = "before unknown exception";
        ok = false;
    }
    t_active_hook = prev;
    pop_reentry();
    if (!ok) return true;
    if (IsSkipOriginal(out)) cancelled = true;
    (void)status;
    return true;
}

void Dispatcher::InvokeAfter(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx) {
    bool has_after = (hook->after != nullptr) || (hook->abi_callback != nullptr && hook->kind == HookKind::After);
    if (!has_after) return;
    if (hook->disabled_this_session) return;
    if (is_reentering(hook.get())) return;
    push_reentry(hook.get());
    ActiveHookScope prev = t_active_hook;
    t_active_hook = {hook->plugin_id.c_str(), hook->hook_id.c_str(), hook->target_id.c_str()};
    HookResult out = MakeContinueResult();
    try {
        if (hook->after) {
            hook->after(ctx, &out);
        } else if (hook->abi_callback) {
            auto fn = reinterpret_cast<orca_hook_callback_t>(hook->abi_callback);
            out.size = static_cast<uint32_t>(sizeof(HookResult));
            out.version = ORCA_HOOK_ABI_VERSION;
            out.action = ORCA_HOOK_ACTION_CONTINUE;
            fn(reinterpret_cast<orca_cpu_context_t*>(ctx), reinterpret_cast<orca_hook_result_t*>(&out), hook->abi_user_data);
        }
    } catch (const std::exception& e){
        hook->disabled_this_session = true;
        hook->last_error = std::string("after exception: ")+e.what();
    } catch (...){
        hook->disabled_this_session = true;
        hook->last_error = "after unknown exception";
    }
    t_active_hook = prev;
    pop_reentry();
    (void)out;
}

void Dispatcher::InvokeReplace(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx, std::function<HookStatusAbi(CpuContext*)> next, bool& called_next, bool& error, HookResult* out_result) {
    called_next = false;
    error = false;
    bool has_replace = (hook->replace != nullptr) || (hook->abi_callback != nullptr && hook->kind == HookKind::Replace);
    if (!has_replace) { error=true; return; }
    if (hook->disabled_this_session) return;
    if (is_reentering(hook.get())) return;
    push_reentry(hook.get());
    ActiveHookScope prev = t_active_hook;
    t_active_hook = {hook->plugin_id.c_str(), hook->hook_id.c_str(), hook->target_id.c_str()};
    HookResult local = MakeContinueResult();
    HookResult* out = out_result ? out_result : &local;
    out->size = static_cast<uint32_t>(sizeof(HookResult));
    out->version = ORCA_HOOK_ABI_VERSION;
    out->action = ORCA_HOOK_ACTION_CONTINUE;
    bool called = false;
    auto next_wrapper = [&](CpuContext* c) -> HookStatusAbi {
        if (called){ error=true; hook->last_error="replace called next twice"; return static_cast<HookStatusAbi>(99); }
        called = true;
        return next(c);
    };
    try {
        if (hook->replace) {
            HookStatusAbi st = hook->replace(ctx, out, next_wrapper);
            (void)st;
            called_next = called;
        } else if (hook->abi_callback) {
            auto fn = reinterpret_cast<orca_replace_callback_t>(hook->abi_callback);
            t_abi_next_fn = [&](CpuContext* c)->HookStatusAbi {
                if (called) { error=true; return static_cast<HookStatusAbi>(99); }
                called=true; return next(c);
            };
            HookStatusAbi st = fn(reinterpret_cast<orca_cpu_context_t*>(ctx), reinterpret_cast<orca_hook_result_t*>(out), &abi_next_shim, hook->abi_user_data);
            t_abi_next_fn = nullptr;
            (void)st;
            called_next = called;
        }
    } catch (const std::exception& e){
        hook->disabled_this_session = true;
        hook->last_error = std::string("replace exception: ")+e.what();
        error = true;
        t_abi_next_fn = nullptr;
    } catch (...){
        hook->disabled_this_session = true;
        hook->last_error = "replace unknown exception";
        error = true;
        t_abi_next_fn = nullptr;
    }
    t_active_hook = prev;
    pop_reentry();
}

void Dispatcher::InvokeRaw(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx){
    bool has_raw = (hook->raw != nullptr) || (hook->abi_callback != nullptr && hook->kind == HookKind::Raw);
    if (!has_raw) return;
    if (hook->disabled_this_session) return;
    if (is_reentering(hook.get())) return;
    push_reentry(hook.get());
    ActiveHookScope prev = t_active_hook;
    t_active_hook = {hook->plugin_id.c_str(), hook->hook_id.c_str(), hook->target_id.c_str()};
    HookResult out = MakeContinueResult();
    try {
        if (hook->raw) {
            hook->raw(ctx, &out);
        } else if (hook->abi_callback) {
            auto fn = reinterpret_cast<orca_hook_callback_t>(hook->abi_callback);
            out.size = static_cast<uint32_t>(sizeof(HookResult));
            out.version = ORCA_HOOK_ABI_VERSION;
            out.action = ORCA_HOOK_ACTION_CONTINUE;
            fn(reinterpret_cast<orca_cpu_context_t*>(ctx), reinterpret_cast<orca_hook_result_t*>(&out), hook->abi_user_data);
        }
    } catch (const std::exception& e){
        hook->disabled_this_session = true;
        hook->last_error = std::string("raw exception: ")+e.what();
    } catch (...){
        hook->disabled_this_session = true;
        hook->last_error = "raw unknown exception";
    }
    t_active_hook = prev;
    pop_reentry();
    (void)out;
}

Dispatcher::DispatchResult Dispatcher::DispatchChain(const std::shared_ptr<TargetChain>& chain, CpuContext* ctx, void* trampoline) {
    if (!chain || !ctx) return DispatchResult::Error;
    chain->active_calls.fetch_add(1);
    // Snapshot under lock: unlink may run concurrently, and shared_ptrs keep
    // unlinked requests alive for the rest of this dispatch.
    std::vector<std::shared_ptr<HookRequest>> snapshot;
    if (registry_) registry_->SnapshotHooks(chain, snapshot);
    else snapshot = chain->hooks;
    std::vector<std::shared_ptr<HookRequest>> befores, replaces, afters, raws;
    for (auto& h : snapshot) {
        if (h->disabled_this_session) continue;
        switch (h->kind){
            case HookKind::Before: befores.push_back(h); break;
            case HookKind::After: afters.push_back(h); break;
            case HookKind::Replace: replaces.push_back(h); break;
            case HookKind::Raw: raws.push_back(h); break;
            default: break;
        }
    }
    if (!raws.empty()){
        for (auto& r : raws) InvokeRaw(r, ctx);
        chain->active_calls.fetch_sub(1);
        return DispatchResult::Continue;
    }
    bool cancelled = false;
    for (auto& b : befores){
        bool cancel=false;
        InvokeBefore(b, ctx, cancel);
        if (cancel) cancelled = true;
    }
    std::function<HookStatusAbi(CpuContext*)> call_original = [&](CpuContext* c)->HookStatusAbi {
        if (cancelled || !trampoline) return static_cast<HookStatusAbi>(0);
        auto fn = reinterpret_cast<void(*)(CpuContext*)>(trampoline);
        ActiveHookScope prev = t_active_hook;
        t_active_hook = {nullptr, nullptr, chain->target.id.c_str()};
        fn(c);
        t_active_hook = prev;
        return static_cast<HookStatusAbi>(0);
    };
    bool replace_error=false;
    std::function<HookStatusAbi(CpuContext*, size_t)> call_replace;
    call_replace = [&](CpuContext* c, size_t idx)->HookStatusAbi {
        if (idx >= replaces.size()){
            if (!cancelled) return call_original(c);
            return static_cast<HookStatusAbi>(0);
        }
        auto& rep = replaces[idx];
        HookResult out = MakeContinueResult();
        bool called_next=false; bool err=false;
        std::function<HookStatusAbi(CpuContext*)> next = [&](CpuContext* nc)->HookStatusAbi {
            return call_replace(nc, idx+1);
        };
        InvokeReplace(rep, c, next, called_next, err, &out);
        if (err) replace_error=true;
        if (!called_next && !err){
            return static_cast<HookStatusAbi>(0);
        } else if (err && !called_next){
            return call_replace(c, idx+1);
        }
        (void)out;
        return static_cast<HookStatusAbi>(0);
    };
    if (!replaces.empty()){
        call_replace(ctx, 0);
    } else {
        if (!cancelled && trampoline){
            call_original(ctx);
        }
    }
    for (auto it = afters.rbegin(); it != afters.rend(); ++it){
        InvokeAfter(*it, ctx);
    }
    chain->active_calls.fetch_sub(1);
    if (replace_error) return DispatchResult::Error;
    return cancelled ? DispatchResult::Cancelled : DispatchResult::Continue;
}

bool Dispatcher::DispatchInline(const TargetInfo& target, CpuContext* ctx, void** trampoline_out) {
    if (!registry_ || !ctx) return false;
    auto chain = registry_->FindChain(target.id, target.rva, target.point);
    if (!chain) return true;
    void* tramp = chain->trampoline;
    if (trampoline_out) *trampoline_out = tramp;
    auto res = DispatchChain(chain, ctx, tramp);
    (void)res;
    return false;
}

void Dispatcher::DispatchMid(const TargetInfo& target, CpuContext* ctx) {
    if (!registry_ || !ctx) return;
    auto chain = registry_->FindChain(target.id, target.rva, target.point);
    if (!chain) return;
    DispatchChain(chain, ctx, nullptr);
}

} // namespace Slic3r::Hook

// Link targets for the assembly stubs: extern "C", noexcept, allocation-free,
// forwarding to the Dispatcher without holding locks.
#include "HookRuntime.hpp"
namespace Slic3r::Hook::Arch::X64 {
extern "C" bool orca_hook_inline_dispatch(void* target_info, ::Slic3r::Hook::CpuContext* ctx, void** trampoline) noexcept {
    if (!target_info || !ctx) {
        if (trampoline) *trampoline = nullptr;
        return true;
    }
    auto* ti = static_cast<::Slic3r::Hook::TargetInfo*>(target_info);
    auto* disp = ::Slic3r::Hook::HookRuntime::Instance().GetDispatcher();
    if (!disp) {
        if (trampoline) *trampoline = nullptr;
        return true;
    }
    // Trampoline out receives the chain's original callable, if any.
    return disp->DispatchInline(*ti, ctx, trampoline);
}
extern "C" void orca_hook_mid_dispatch(void* target_info, ::Slic3r::Hook::CpuContext* ctx) noexcept {
    if (!target_info || !ctx) return;
    auto* ti = static_cast<::Slic3r::Hook::TargetInfo*>(target_info);
    auto* disp = ::Slic3r::Hook::HookRuntime::Instance().GetDispatcher();
    if (!disp) return;
    disp->DispatchMid(*ti, ctx);
}
#ifdef _WIN32
extern "C" void orca_hook_call_trampoline(void* trampoline, ::Slic3r::Hook::CpuContext* ctx) noexcept {
    if (!trampoline || !ctx) return;
    auto fn = reinterpret_cast<void(*)(::Slic3r::Hook::CpuContext*)>(trampoline);
    fn(ctx);
}
#endif
} // namespace Slic3r::Hook::Arch::X64
