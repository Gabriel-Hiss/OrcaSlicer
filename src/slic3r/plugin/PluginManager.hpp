#ifndef slic3r_PluginManager_hpp_
#define slic3r_PluginManager_hpp_

#include "PluginDescriptor.hpp"
#include "PluginLoader.hpp"

#include <boost/filesystem/path.hpp>

#include <atomic>
#include <condition_variable>
#include <functional>
#include <map>
#include <memory>
#include <mutex>
#include <string>
#include <unordered_set>
#include <vector>

namespace Slic3r {

struct LoadedPlugin
{
    PluginDescriptor descriptor;
    void* native_handle = nullptr; // HMODULE / dl handle; only for native runtime
    void* entry_fn = nullptr;
    void* exit_fn = nullptr;
    bool jvm_loaded = false;

    bool is_loaded() const { return native_handle != nullptr || jvm_loaded; }

    LoadedPlugin() = default;
    LoadedPlugin(const LoadedPlugin& ) = delete;
    LoadedPlugin& operator=(const LoadedPlugin& ) = delete;
    LoadedPlugin(LoadedPlugin&& o) noexcept
        : descriptor(std::move(o.descriptor)),
          native_handle(o.native_handle),
          entry_fn(o.entry_fn),
          exit_fn(o.exit_fn),
          jvm_loaded(o.jvm_loaded)
    {
        o.native_handle = nullptr;
        o.entry_fn = nullptr;
        o.exit_fn = nullptr;
        o.jvm_loaded = false;
    }
    LoadedPlugin& operator=(LoadedPlugin&& o) noexcept
    {
        if (this != &o) {
            descriptor = std::move(o.descriptor);
            native_handle = o.native_handle;
            entry_fn = o.entry_fn;
            exit_fn = o.exit_fn;
            jvm_loaded = o.jvm_loaded;
            o.native_handle = nullptr;
            o.entry_fn = nullptr;
            o.exit_fn = nullptr;
            o.jvm_loaded = false;
        }
        return *this;
    }
    ~LoadedPlugin() = default;
};

class PluginManager
{
public:
    using PluginLifecycleCompleteFn = std::function<void(const std::string&)>;

    static PluginManager& instance();
    ~PluginManager();

    bool initialize();
    void shutdown();
    void set_shutting_down();

    void discover_plugins(bool async = false, bool clear = false);
    void rescan_plugins();

    std::vector<PluginDescriptor> get_plugin_descriptors(bool include_invalid = false) const;
    bool try_get_plugin_descriptor(const std::string& plugin_id, PluginDescriptor& out) const;
    bool try_get_valid_plugin_descriptor(const std::string& plugin_id, PluginDescriptor& out) const;
    std::vector<std::string> get_enabled_plugin_ids() const;

    bool is_plugin_loaded(const std::string& plugin_id) const;
    bool is_plugin_enabled(const std::string& plugin_id) const;
    bool set_plugin_enabled(const std::string& plugin_id, bool enabled, std::string& error);
    bool load_plugin(const std::string& plugin_id, std::string& error);
    bool unload_plugin(const std::string& plugin_id, std::string& error);
    bool reload_plugin(const std::string& plugin_id, std::string& error);
    void unload_all_plugins();

    std::string get_plugin_load_error(const std::string& plugin_id) const;

    void subscribe_on_load_callback(PluginLifecycleCompleteFn fn);
    void subscribe_on_unload_callback(PluginLifecycleCompleteFn fn);

    bool install_plugin(const boost::filesystem::path& filepath, std::string& error);
    bool install_plugin(const boost::filesystem::path& filepath, PluginDescriptor& plugin_descriptor, std::string& error);
    bool inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                      PluginDescriptor& plugin_descriptor,
                                      bool& existing_installation,
                                      std::string& error) const;

    bool set_plugin_error(const std::string& plugin_id, std::string error);
    bool clear_plugin_error(const std::string& plugin_id);

    bool delete_plugin(const std::string& plugin_id, std::string& error);
    void open_plugin_folder(const std::string& plugin_id) const;

    bool is_discovery_complete() const;
    std::string get_discovery_error() const;
    bool wait_for_discovery(std::chrono::milliseconds timeout, std::string& error) const;

    std::string current_build_id() const;
    bool is_jvm_available() const;
    std::string jvm_error() const;

    void autoload_enabled_plugins();

private:
    PluginManager() = default;
    PluginManager(const PluginManager&) = delete;
    PluginManager& operator=(const PluginManager&) = delete;

    LoadedPlugin* find_plugin_locked(const std::string& plugin_id);
    const LoadedPlugin* find_plugin_locked(const std::string& plugin_id) const;
    void merge_discovered_plugins(std::vector<PluginDescriptor> discovered, bool clear);
    void run_discovery(bool async, bool clear);

    std::vector<PluginLifecycleCompleteFn> copy_callbacks(bool is_load) const;
    void run_on_load_callbacks(const std::string& k);
    void run_on_unload_callbacks(const std::string& k);

    mutable std::mutex m_mutex;
    std::vector<LoadedPlugin> m_plugins;
    std::map<std::string, std::string> m_load_errors;

    std::vector<PluginLifecycleCompleteFn> m_load_callbacks;
    std::vector<PluginLifecycleCompleteFn> m_unload_callbacks;

    std::atomic<bool> m_initialized{false};
    std::atomic<bool> m_shutting_down{false};

    bool m_discovery_complete = false;
    std::string m_discovery_error;
    mutable std::condition_variable m_discovery_cv;
    std::string m_jvm_error;
};

} // namespace Slic3r

#endif // slic3r_PluginManager_hpp_
