#pragma once

#include "CpuContext.hpp"
#include "HookBackend.hpp"
#include "HookDefs.hpp"
#include "HookManifest.hpp"
#include <atomic>
#include <functional>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>
namespace Slic3r::Hook {

// One chain per target, sorted by priority desc, plugin_id asc, declare order asc.
// No callback runs while the registry mutex is held.

struct HookRequest {
    PluginId plugin_id;
    HookId hook_id;
    TargetId target_id;
    uint64_t target_rva = 0; // 0 means resolve via target_id
    HookPoint point = HookPoint::ENTRY;
    HookKind kind = HookKind::Before;
    int32_t priority = kDefaultPriority;
    uint32_t declare_order = 0;

    // Point-specific details
    InvokeDetail invoke{};
    OffsetDetail offset{};
    VTableDetail vtable{};
    ImportDetail import{};

    // Callbacks take CpuContext plus out HookResult; BEFORE cancels with SKIP_ORIGINAL.
    using BeforeFn  = std::function<HookStatusAbi(CpuContext*, HookResult*)>;
    using AfterFn   = std::function<HookStatusAbi(CpuContext*, HookResult*)>;
    using ReplaceFn = std::function<HookStatusAbi(CpuContext*, HookResult*, std::function<HookStatusAbi(CpuContext*)>)>;
    using RawFn     = std::function<HookStatusAbi(CpuContext*, HookResult*)>;

    // Exactly one of these is set, according to kind.
    BeforeFn before;
    AfterFn after;
    ReplaceFn replace;
    RawFn raw;

    // Raw ABI callback and user data for orca_hook_request_t installs.
    void* abi_callback = nullptr;
    void* abi_user_data = nullptr;

    HookHandle backend_handle = nullptr;
    void* trampoline = nullptr;

    // Cleared on the next reload or start.
    bool disabled_this_session = false;
    std::string last_error;
};

struct TargetChain {
    TargetInfo target{};
    std::vector<std::shared_ptr<HookRequest>> hooks;
    std::atomic<uint32_t> active_calls{0};
    HookHandle backend_install_handle = nullptr;
    // RETURN installs one breakpoint-trap per RET site sharing this chain.
    std::vector<HookHandle> extra_backend_handles;
    void* trampoline = nullptr;
};

// A plugin's hooks install atomically; failure rolls back without invoking its entry.
class HookRegistry {
public:
    explicit HookRegistry(HookManifest* manifest, HookBackend* backend)
        : manifest_(manifest), backend_(backend) {}

    HookRegistry(const HookRegistry&) = delete;
    HookRegistry& operator=(const HookRegistry&) = delete;

    // Resolve a request to an absolute target; OFFSET requires an instruction boundary.
    bool ResolveTarget(const HookRequest& req, TargetInfo& out, std::string& error) const;

    // Register one hook in an open transaction.
    bool InstallOne(const std::shared_ptr<HookRequest>& req, std::string& error);

    // Install a plugin's hooks atomically; any failure rolls back.
    bool InstallTransaction(const PluginId& plugin_id,
                            std::vector<std::shared_ptr<HookRequest>> requests,
                            std::string& error);

    // Unlink first, drain active calls with timeout, then unpatch.
    // A timed-out drain leaves the module resident and returns RestartRequired.
    HookStatus RemovePluginHooks(const PluginId& plugin_id, uint32_t drain_timeout_ms, std::string& error);

    // Disable one hook until the next reload or start.
    void DisableHookForSession(const HookId& hook_id);
    std::shared_ptr<TargetChain> FindChain(const TargetId& target_id, uint64_t rva, HookPoint point) const;
    std::vector<std::shared_ptr<TargetChain>> AllChains() const;
    // Copy under lock so dispatch never reads the live vector during unlink.
    void SnapshotHooks(const std::shared_ptr<TargetChain>& chain,
                       std::vector<std::shared_ptr<HookRequest>>& out) const;
    // Collect the primary plus RETURN extra handles owning `handle`, under lock.
    bool FindBackendHandles(HookHandle handle, std::vector<HookHandle>& out) const;

    // Session disables clear on the next start or reload.
    void ResetSessionDisables(const PluginId& plugin_id);
    void ResetAllSessionDisables();
    uint32_t ActiveCallsForPlugin(const PluginId& pid) const;
    uint32_t ActiveCallsForHook(const HookId& hid) const;
    // Test accessors for fault injection.
    HookManifest* manifest() const { return manifest_; }
    HookBackend* backend() const { return backend_; }

private:
    mutable std::mutex mutex_;
    // Canonical target key: id, point, and discriminant (slot, offset, ordinal).
    std::unordered_map<std::string, std::shared_ptr<TargetChain>> chains_;
    std::unordered_map<HookId, std::atomic<uint32_t>> hook_active_;
    std::unordered_map<PluginId, std::atomic<uint32_t>> plugin_active_;

    HookManifest* manifest_ = nullptr;
    HookBackend*  backend_ = nullptr;

    std::string ChainKey(const TargetInfo& t) const;
    void SortChain(TargetChain& chain);

    // Call only with mutex_ held.
    bool InstallOneLocked(const std::shared_ptr<HookRequest>& req, std::string& error);
    void UnlinkHookLocked(const HookId& hid);
    // Manifest map fast path, else Zydis decode of pristine bytes.
    bool ValidateOffsetBoundary(const ManifestSymbol& sym, uint64_t abs_rva, std::string& error) const;
    // One breakpoint-trap per RET site. Patching runs without the lock;
    // publishing the handles takes it. Extras append to site_handles_out for rollback.
    bool InstallReturnSites(const std::shared_ptr<TargetChain>& chain,
                            const std::shared_ptr<HookRequest>& req,
                            const TargetInfo& target,
                            std::vector<HookHandle>& site_handles_out,
                            std::string& error);
    bool DrainAndRemoveLocked(const std::shared_ptr<TargetChain>& chain,
                               const PluginId& plugin_id,
                               uint32_t timeout_ms,
                               std::string& error,
                               bool& restart_required);
};

} // namespace Slic3r::Hook
