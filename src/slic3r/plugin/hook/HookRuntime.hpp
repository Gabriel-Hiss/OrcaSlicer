#pragma once

#include "../../../../sdk/plugin_v1/abi/orca_hook_api.h"
#include "Dispatch.hpp"
#include "HookBackend.hpp"
#include "HookManifest.hpp"
#include "HookRegistry.hpp"
#include <memory>
#include <string>

namespace Slic3r::Hook {

// Owns manifest, backend, registry, active-call counters, and drain logic.
// Exposed to plugins via the C ABI host table. Registry mutations are
// transactional; no callbacks run under locks.

class HookRuntime {
public:
    static HookRuntime& Instance();

    HookRuntime(const HookRuntime&) = delete;
    HookRuntime& operator=(const HookRuntime&) = delete;

    // Call once before GUI/slicing; Shutdown disarms all chains first.
    bool Initialize(const std::string& runtime_manifest_path, std::string& error);
    void Shutdown() noexcept;

    bool IsInitialized() const noexcept { return initialized_; }
    bool IsSupported() const noexcept { return backend_ && backend_->IsSupported(); }
    const BuildId& ActiveBuildId() const { return active_build_; }
    HookManifest* Manifest() const { return manifest_.get(); }
    HookRegistry* Registry() const { return registry_.get(); }
    HookBackend* Backend() const { return backend_.get(); }

    // The manifest build_id must exactly match the running image.
    bool ValidateBuildId(std::string& error) const;

    // Install a plugin's hooks transactionally; any failure rolls back
    // without invoking the plugin entry.
    bool InstallPluginHooks(const PluginId& plugin_id,
                            std::vector<std::shared_ptr<HookRequest>> requests,
                            std::string& error);

    // Drain active calls with timeout; a timed-out drain leaves the module
    // resident and returns RestartRequired.
    HookStatus RemovePluginHooks(const PluginId& plugin_id, uint32_t timeout_ms, std::string& error);
    HookStatus DisablePluginHooks(const PluginId& plugin_id, uint32_t timeout_ms, std::string& error) {
        return RemovePluginHooks(plugin_id, timeout_ms, error);
    }

    // A throwing hook is disabled until the next start or reload.
    void DisableHookForSession(const HookId& hook_id) { if (registry_) registry_->DisableHookForSession(hook_id); }
    void ResetSessionDisables(const PluginId& pid) { if (registry_) registry_->ResetSessionDisables(pid); }

    // No lock is held during callbacks; reentry and active-call tracking inside.
    Dispatcher* GetDispatcher() const { return dispatcher_.get(); }
    // Bridged to plugins through the host table.
    bool ReadMemory(const void* src, void* dst, size_t sz) const noexcept;
    bool WriteMemory(void* dst, const void* src, size_t sz) const noexcept;
    bool ProtectMemory(void* addr, size_t sz, MemProt prot, MemProt* old) const noexcept;
    void FlushICache(void* addr, size_t sz) const noexcept;
    // Plugin host table (orca_host_api_v1).
    const orca_host_api_v1_t* GetHostApi() const noexcept;
    // Scopes plugin_id while orca_plugin_entry_v1 synchronously installs hooks.
    static void SetCurrentPluginForAbi(const PluginId& id);
    static PluginId GetCurrentPluginForAbi();

    // Thread-local install error naming the failed hook and reason.
    static void SetLastHookError(const std::string& err);
    static std::string GetLastHookError();
    static const char* HookStatusName(orca_hook_status_t status) noexcept;
    std::string GetLastError() const { return last_error_; }

    // Test-only injection.
    void SetManifestForTesting(std::unique_ptr<HookManifest> m);
    void SetBackendForTesting(std::unique_ptr<HookBackend> b);

private:
    // Used when Initialize gets an empty path.
    static std::string FindDefaultManifestPath();
    HookRuntime() = default;
    ~HookRuntime() = default;

    std::unique_ptr<HookManifest> manifest_;
    std::unique_ptr<HookBackend>  backend_;
    std::unique_ptr<HookRegistry> registry_;
    std::unique_ptr<Dispatcher>   dispatcher_;
    BuildId active_build_{};
    bool initialized_ = false;
    std::string last_error_;
};

} // namespace Slic3r::Hook
