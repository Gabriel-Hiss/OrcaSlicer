#pragma once

#include "CpuContext.hpp"
#include "HookRegistry.hpp"
#include <functional>

namespace Slic3r::Hook {

// One chain per target: before runs high to low and may cancel, replace
// wraps the original outermost-first via next(), after runs low to high even
// when cancelled. A reentering hook skips only its own record. No registry
// lock is held during callbacks; a throwing hook is disabled for the session.
// An access violation stays fatal; the faulting hook is annotated thread-locally.

class Dispatcher {
public:
    explicit Dispatcher(HookRegistry* registry) : registry_(registry) {}

    // Called from the assembly stubs with the trapped context and target.
    // Inline returns whether the stub should still call the original;
    // it also reports the original callable through trampoline_out.
    bool DispatchInline(const TargetInfo& target, CpuContext* ctx, void** trampoline_out);

    // Trap-site dispatcher (ENTRY/OFFSET/INVOKE/RETURN).
    void DispatchMid(const TargetInfo& target, CpuContext* ctx);
    // Core chain execution. Called with already-resolved chain copy (no lock).
    enum class DispatchResult { Continue, Cancelled, Error };
    DispatchResult DispatchChain(const std::shared_ptr<TargetChain>& chain, CpuContext* ctx, void* trampoline);

private:
    HookRegistry* registry_ = nullptr;
    // Invoke helpers isolate callback exceptions; chain control flows via out_result.
    bool InvokeBefore(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx, bool& cancelled);
    void InvokeAfter(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx);
    void InvokeReplace(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx, std::function<HookStatusAbi(CpuContext*)> next, bool& called_next, bool& error, HookResult* out_result);
    void InvokeRaw(const std::shared_ptr<HookRequest>& hook, CpuContext* ctx);
};

} // namespace Slic3r::Hook
