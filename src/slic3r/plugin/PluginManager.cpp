#include "PluginManager.hpp"

#include "PluginDescriptor.hpp"
#include "PluginFsUtils.hpp"
#include "PluginLoader.hpp"

#include "slic3r/plugin/package/PackageReader.hpp"
#include "slic3r/plugin/package/Hash.hpp"
#include "slic3r/plugin/package/InstallState.hpp"
#include "slic3r/plugin/package/PluginMetadata.hpp"
#include "slic3r/plugin/hook/HookRuntime.hpp"
#include "slic3r/plugin/runtime/jvm/JvmPluginRuntime.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/iostream.hpp>

#include <algorithm>
#include <chrono>
#include <condition_variable>
#include <mutex>
#include <thread>
#include <unordered_set>
#include <vector>

#ifdef _WIN32
#include <windows.h>
#include <shellapi.h>
#endif
#include <boost/nowide/convert.hpp>

namespace Slic3r {

namespace fs = boost::filesystem;

static std::string normalize_id(const std::string& id)
{
    std::string out = id;
    std::transform(out.begin(), out.end(), out.begin(), [](unsigned char c){ return std::tolower(c); });
    return out;
}

PluginManager& PluginManager::instance()
{
    static PluginManager inst;
    return inst;
}

PluginManager::~PluginManager()
{
    shutdown();
}

bool PluginManager::initialize()
{
    bool expected = false;
    if (!m_initialized.compare_exchange_strong(expected, true)) {
        return true;
    }
    m_shutting_down.store(false);
    // Expose data_dir to native plugins that read it from the environment.
    {
        std::string dd = data_dir();
        if (!dd.empty()) {
#ifdef _WIN32
            _putenv_s("ORCA_DATA_DIR", dd.c_str());
            _putenv_s("SLIC3R_DATA_DIR", dd.c_str());
#else
            ::setenv("ORCA_DATA_DIR", dd.c_str(), 1);
            ::setenv("SLIC3R_DATA_DIR", dd.c_str(), 1);
#endif
            BOOST_LOG_TRIVIAL(info) << "[orca-plugins] ORCA_DATA_DIR set to " << dd << " for native plugins";
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_discovery_complete = false;
        m_discovery_error.clear();
        m_jvm_error.clear();
    }
    // Initialize HookRuntime so build-id checks see the current build.
    {
        std::string hook_err;
        if (!Hook::HookRuntime::Instance().IsInitialized()) {
            if (!Hook::HookRuntime::Instance().Initialize("", hook_err)) {
                BOOST_LOG_TRIVIAL(error) << "HookRuntime Initialize failed: " << hook_err;
                try { boost::nowide::cerr << "[orca-plugins] HookRuntime Initialize failed: " << hook_err << std::endl; } catch (...) {}
            } else {
                BOOST_LOG_TRIVIAL(info) << "HookRuntime initialized: build_id=" << plugin_loader::current_build_id_string();
            }
        } else {
            BOOST_LOG_TRIVIAL(info) << "HookRuntime already initialized: build_id=" << plugin_loader::current_build_id_string();
        }
    }
    run_discovery(false, true);
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!m_discovery_error.empty()) {
            BOOST_LOG_TRIVIAL(error) << "Plugin discovery failed: " << m_discovery_error;
            try { boost::nowide::cerr << "[orca-plugins] discovery failed: " << m_discovery_error << std::endl; } catch (...) {}
        } else {
            BOOST_LOG_TRIVIAL(info) << "Plugin discovery complete: " << m_plugins.size() << " plugin(s) found";
        }
    }
    return true;
}

void PluginManager::shutdown()
{
    bool was = m_initialized.exchange(false);
    if (!was) {
        m_shutting_down.store(true);
        return;
    }
    m_shutting_down.store(true);
    unload_all_plugins();
    // Disarm hooks before shutting down the hook runtime and JVM.
    try {
        Hook::HookRuntime::Instance().Shutdown();
    } catch (...) {}
    try {
        Plugin::Jvm::JvmPluginRuntime::instance().shutdown();
    } catch (...) {}
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_discovery_complete = true;
        m_discovery_error.clear();
    }
    m_discovery_cv.notify_all();
}

void PluginManager::set_shutting_down()
{
    m_shutting_down.store(true);
}

LoadedPlugin* PluginManager::find_plugin_locked(const std::string& plugin_id)
{
    for (auto &p : m_plugins) {
        if (p.descriptor.id == plugin_id) return &p;
    }
    return nullptr;
}

const LoadedPlugin* PluginManager::find_plugin_locked(const std::string& plugin_id) const
{
    for (auto &p : m_plugins) {
        if (p.descriptor.id == plugin_id) return &p;
    }
    return nullptr;
}

void PluginManager::merge_discovered_plugins(std::vector<PluginDescriptor> discovered, bool clear)
{
    // Collect ids before moving descriptors.
    std::unordered_set<std::string> disc_ids;
    disc_ids.reserve(discovered.size() * 2 + 1);
    for (auto &d : discovered) disc_ids.insert(d.id);

    for (auto &d : discovered) {
        LoadedPlugin* existing = find_plugin_locked(d.id);
        if (existing) {
            bool was_loaded = existing->is_loaded();
            bool was_restart = existing->descriptor.restart_required;
            void* h = existing->native_handle;
            void* ef = existing->entry_fn;
            void* xf = existing->exit_fn;
            bool jvm = existing->jvm_loaded;
            // Overwrite the descriptor but keep native handles if loaded.
            existing->descriptor = std::move(d);
            if (was_loaded) {
                existing->native_handle = h;
                existing->entry_fn = ef;
                existing->exit_fn = xf;
                existing->jvm_loaded = jvm;
                if (was_restart) existing->descriptor.restart_required = true;
            }
        } else {
            LoadedPlugin np;
            np.descriptor = std::move(d);
            np.native_handle = nullptr;
            np.entry_fn = nullptr;
            np.exit_fn = nullptr;
            np.jvm_loaded = false;
            m_plugins.emplace_back(std::move(np));
        }
    }

    if (clear) {
        auto it = m_plugins.begin();
        while (it != m_plugins.end()) {
            if (disc_ids.find(it->descriptor.id) == disc_ids.end()) {
                // Keep loaded entries until restart.
                if (it->is_loaded() || it->descriptor.restart_required) { ++it; continue; }
                m_load_errors.erase(it->descriptor.id);
                it = m_plugins.erase(it);
            } else {
                ++it;
            }
        }
    } else {
        auto it = m_plugins.begin();
        while (it != m_plugins.end()) {
            if (disc_ids.find(it->descriptor.id) == disc_ids.end() && !it->is_loaded() && !it->descriptor.restart_required) {
                m_load_errors.erase(it->descriptor.id);
                it = m_plugins.erase(it);
            } else {
                ++it;
            }
        }
    }
}

std::vector<PluginManager::PluginLifecycleCompleteFn> PluginManager::copy_callbacks(bool is_load) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (is_load) return m_load_callbacks;
    return m_unload_callbacks;
}

void PluginManager::run_on_load_callbacks(const std::string& k)
{
    auto cbs = copy_callbacks(true);
    for (auto &fn : cbs) {
        try { fn(k); } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "on_load callback threw: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "on_load callback threw unknown";
        }
    }
}

void PluginManager::run_on_unload_callbacks(const std::string& k)
{
    auto cbs = copy_callbacks(false);
    for (auto &fn : cbs) {
        try { fn(k); } catch (const std::exception& ex) {
            BOOST_LOG_TRIVIAL(warning) << "on_unload callback threw: " << ex.what();
        } catch (...) {
            BOOST_LOG_TRIVIAL(warning) << "on_unload callback threw unknown";
        }
    }
}

void PluginManager::run_discovery(bool async, bool clear)
{
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_discovery_complete = false;
        m_discovery_error.clear();
    }

    auto task = [this, clear]() {
        std::vector<PluginDescriptor> discovered;
        std::string err;
        try {
            auto dirs = get_plugin_directories();
            discovered = discover_plugin_packages(dirs, err);
        } catch (const std::exception& ex) {
            err = ex.what();
        } catch (...) {
            err = "Unknown discovery error";
        }
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            if (!err.empty()) m_discovery_error = err;
            merge_discovered_plugins(std::move(discovered), clear);
            m_discovery_complete = true;
        }
        m_discovery_cv.notify_all();
    };

    if (async) {
        std::thread(task).detach();
    } else {
        task();
    }
}

void PluginManager::discover_plugins(bool async, bool clear)
{
    if (m_shutting_down.load()) {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_discovery_complete = true;
        m_discovery_error.clear();
        m_discovery_cv.notify_all();
        return;
    }
    run_discovery(async, clear);
}

void PluginManager::rescan_plugins()
{
    discover_plugins(false, false);
}

bool PluginManager::is_discovery_complete() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_discovery_complete;
}

std::string PluginManager::get_discovery_error() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    return m_discovery_error;
}

bool PluginManager::wait_for_discovery(std::chrono::milliseconds timeout, std::string& error) const
{
    std::unique_lock<std::mutex> lk(m_mutex);
    if (m_discovery_complete) {
        error = m_discovery_error;
        return m_discovery_error.empty();
    }
    if (timeout == std::chrono::milliseconds::max()) {
        m_discovery_cv.wait(lk, [this]{ return m_discovery_complete; });
    } else {
        if (!m_discovery_cv.wait_for(lk, timeout, [this]{ return m_discovery_complete; })) {
            error = "Discovery timeout";
            return false;
        }
    }
    error = m_discovery_error;
    return m_discovery_error.empty();
}

std::vector<PluginDescriptor> PluginManager::get_plugin_descriptors(bool include_invalid) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<PluginDescriptor> out;
    out.reserve(m_plugins.size());
    for (auto &p : m_plugins) {
        if (!include_invalid) {
            if (p.descriptor.is_invalid_package() || p.descriptor.has_error()) continue;
        }
        out.push_back(p.descriptor);
    }
    return out;
}

bool PluginManager::try_get_plugin_descriptor(const std::string& plugin_id, PluginDescriptor& out) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const LoadedPlugin* f = find_plugin_locked(plugin_id);
    if (!f) return false;
    out = f->descriptor;
    return true;
}

bool PluginManager::try_get_valid_plugin_descriptor(const std::string& plugin_id, PluginDescriptor& out) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const LoadedPlugin* f = find_plugin_locked(plugin_id);
    if (!f) return false;
    if (f->descriptor.is_invalid_package() || f->descriptor.has_error()) return false;
    if (!f->descriptor.metadata_valid) return false;
    out = f->descriptor;
    return true;
}

std::vector<std::string> PluginManager::get_enabled_plugin_ids() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    std::vector<std::string> out;
    out.reserve(m_plugins.size());
    for (auto &p : m_plugins) if (p.descriptor.enabled) out.push_back(p.descriptor.id);
    return out;
}

bool PluginManager::is_plugin_loaded(const std::string& plugin_id) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const LoadedPlugin* f = find_plugin_locked(plugin_id);
    return f && f->is_loaded();
}

bool PluginManager::is_plugin_enabled(const std::string& plugin_id) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    const LoadedPlugin* f = find_plugin_locked(plugin_id);
    return f && f->descriptor.enabled;
}

std::string PluginManager::get_plugin_load_error(const std::string& plugin_id) const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    auto it = m_load_errors.find(plugin_id);
    if (it != m_load_errors.end()) return it->second;
    const LoadedPlugin* f = find_plugin_locked(plugin_id);
    if (f) return f->descriptor.error;
    return {};
}

bool PluginManager::set_plugin_enabled(const std::string& plugin_id, bool enabled, std::string& error)
{
    error.clear();
    PluginDescriptor copy;
    fs::path plugin_dir;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin not found: " + plugin_id; return false; }
        copy = f->descriptor;
        plugin_dir = resolve_plugin_root_from_descriptor(copy);
    }
    if (plugin_dir.empty()) {
        plugin_dir = fs::path(get_orca_plugins_dir()) / plugin_id;
    }
    bool ok = false;
    {
        copy.enabled = enabled;
        ok = write_install_state(plugin_dir, copy, enabled);
        if (!ok) {
            error = "Failed to write install state for " + plugin_id;
            return false;
        }
    }
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (f) f->descriptor.enabled = enabled;
    }
    return true;
}

bool PluginManager::load_plugin(const std::string& plugin_id, std::string& error)
{
    error.clear();
    if (m_shutting_down.load()) { error = "Shutting down"; return false; }

    PluginDescriptor desc_copy;
    bool found = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin not found: " + plugin_id; return false; }
        if (f->is_loaded()) return true;
        desc_copy = f->descriptor;
        found = true;
    }
    if (!found) return false;

    // Hash contract: verify SHA before any execution
    if (!desc_copy.artifact_path.empty()) {
        fs::path art(desc_copy.artifact_path);
        boost::system::error_code ec;
        if (!fs::exists(art, ec)) {
            error = "Artifact not found: " + desc_copy.artifact_path;
            set_plugin_error(plugin_id, error);
            std::lock_guard<std::mutex> lk(m_mutex);
            m_load_errors[plugin_id] = error;
            return false;
        }
        std::string hash_err;
        std::string computed = Plugin::Package::sha256_file_hex(art, hash_err);
        if (!hash_err.empty() || computed.empty()) {
            error = "Failed to compute hash for " + plugin_id + ": " + hash_err;
            set_plugin_error(plugin_id, error);
            std::lock_guard<std::mutex> lk(m_mutex);
            m_load_errors[plugin_id] = error;
            return false;
        }
        // A stored hash must match the computed hash.
        if (!desc_copy.artifact_hash.empty() && computed != desc_copy.artifact_hash) {
            error = "Hash mismatch for " + plugin_id + " stored " + desc_copy.artifact_hash + " computed " + computed;
            set_plugin_error(plugin_id, error);
            std::lock_guard<std::mutex> lk(m_mutex);
            m_load_errors[plugin_id] = error;
            return false;
        }
        fs::path plugin_dir = resolve_plugin_root_from_descriptor(desc_copy);
        if (plugin_dir.empty()) plugin_dir = fs::path(get_orca_plugins_dir()) / plugin_id;
        Plugin::Package::InstallState ist;
        std::string ist_err;
        if (Plugin::Package::read_install_state(plugin_dir, ist, ist_err)) {
            if (!ist.hash.empty() && ist.hash != computed) {
                error = "Hash mismatch for " + plugin_id + " stored " + ist.hash + " computed " + computed;
                set_plugin_error(plugin_id, error);
                std::lock_guard<std::mutex> lk(m_mutex);
                m_load_errors[plugin_id] = error;
                return false;
            }
        }
    }

    if (desc_copy.restart_required) {
        error = "Plugin '" + plugin_id + "' requires restart.";
        set_plugin_error(plugin_id, error);
        std::lock_guard<std::mutex> lk(m_mutex);
        m_load_errors[plugin_id] = error;
        return false;
    }

    bool ok = false;
    std::string load_err;
    {
        LoadedPlugin tmp;
        tmp.descriptor = desc_copy;
        ok = plugin_loader::load_plugin(tmp, load_err);
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin disappeared during load: " + plugin_id; return false; }
        if (ok) {
            f->native_handle = tmp.native_handle;
            f->entry_fn = tmp.entry_fn;
            f->exit_fn = tmp.exit_fn;
            f->jvm_loaded = tmp.jvm_loaded;
            f->descriptor.error.clear();
            f->descriptor.restart_required = false;
            tmp.native_handle = nullptr;
            tmp.jvm_loaded = false;
            m_load_errors.erase(plugin_id);
        } else {
            f->descriptor.error = load_err;
            m_load_errors[plugin_id] = load_err;
            error = load_err;
        }
    }
    if (ok) {
        run_on_load_callbacks(plugin_id);
        return true;
    } else {
        if (error.empty()) error = load_err;
        return false;
    }
}

bool PluginManager::unload_plugin(const std::string& plugin_id, std::string& error)
{
    error.clear();
    bool was_loaded = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin not found: " + plugin_id; return false; }
        was_loaded = f->is_loaded();
        if (!was_loaded) return true;
    }
    std::string unload_err;
    bool need_restart = false;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin not found: " + plugin_id; return false; }
        // Unload drains HookRuntime before releasing the module.
        plugin_loader::unload_plugin(*f, unload_err);
        if (f->descriptor.restart_required) {
            need_restart = true;
            error = unload_err.empty() ? "Unload requires restart" : unload_err;
            m_load_errors[plugin_id] = error;
        } else {
            m_load_errors.erase(plugin_id);
            f->descriptor.error.clear();
        }
    }
    if (!need_restart) {
        run_on_unload_callbacks(plugin_id);
    }
    return !need_restart;
}

bool PluginManager::reload_plugin(const std::string& plugin_id, std::string& error)
{
    error.clear();
    PluginDescriptor before;
    if (!try_get_plugin_descriptor(plugin_id, before)) {
        error = "Plugin not found: " + plugin_id;
        return false;
    }

    std::string unload_err;
    bool was_loaded = is_plugin_loaded(plugin_id);
    if (was_loaded) {
        if (!unload_plugin(plugin_id, unload_err)) {
            error = unload_err;
            return false; // restart_required case
        }
    }

    // Re-discover packages to pick up new artifacts.
    {
        std::string disc_err;
        auto dirs = get_plugin_directories();
        auto discovered = discover_plugin_packages(dirs, disc_err);
        std::lock_guard<std::mutex> lk(m_mutex);
        if (!disc_err.empty()) m_discovery_error = disc_err;
        merge_discovered_plugins(std::move(discovered), false);
    }

    // Discovery already rejects hash mismatches; refuse to load them here.
    PluginDescriptor after;
    if (!try_get_plugin_descriptor(plugin_id, after)) {
        error = "Plugin not found after rescan: " + plugin_id;
        return false;
    }
    if (after.has_error()) {
        error = after.error;
        std::lock_guard<std::mutex> lk(m_mutex);
        m_load_errors[plugin_id] = error;
        return false;
    }

    return load_plugin(plugin_id, error);
}

void PluginManager::unload_all_plugins()
{
    std::vector<std::string> ids;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto &p : m_plugins) if (p.is_loaded()) ids.push_back(p.descriptor.id);
    }
    for (auto &id : ids) {
        std::string err;
        unload_plugin(id, err);
    }
}

bool PluginManager::inspect_local_plugin_package(const boost::filesystem::path& filepath,
                                                 PluginDescriptor& plugin_descriptor,
                                                 bool& existing_installation,
                                                 std::string& error) const
{
    error.clear();
    existing_installation = false;
    return plugin_loader::inspect_local_plugin_package(filepath, plugin_descriptor, existing_installation, error);
}

bool PluginManager::install_plugin(const boost::filesystem::path& filepath, std::string& error)
{
    PluginDescriptor dummy;
    return install_plugin(filepath, dummy, error);
}

bool PluginManager::install_plugin(const boost::filesystem::path& filepath, PluginDescriptor& plugin_descriptor, std::string& error)
{
    error.clear();
    plugin_descriptor = PluginDescriptor{};

    // Inspect before any write to reject path traversal ids.
    PluginDescriptor inspected;
    bool existing = false;
    std::string insp_err;
    if (!plugin_loader::inspect_local_plugin_package(filepath, inspected, existing, insp_err)) {
        error = insp_err;
        return false;
    }

    if (!Plugin::Package::is_safe_plugin_id_for_fs(inspected.id) || !Plugin::Package::is_valid_plugin_id(inspected.id)) {
        error = "Invalid plugin id: " + inspected.id;
        return false;
    }
    std::string ext;
    if (!Plugin::Package::is_allowed_artifact_extension(filepath, ext)) {
        error = "Unsupported artifact extension: " + filepath.string();
        return false;
    }

    // Refuse the update while a restart is pending.
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(inspected.id);
        if (f && f->descriptor.restart_required) {
            error = "Plugin '" + inspected.id + "' requires restart before it can be updated. Restart the application and try again.";
            return false;
        }
    }

    // Unload first so the artifact can be overwritten on Windows.
    {
        bool loaded = false;
        {
            std::lock_guard<std::mutex> lk(m_mutex);
            LoadedPlugin* f = find_plugin_locked(inspected.id);
            loaded = f && f->is_loaded();
        }
        if (loaded) {
            std::string unload_err;
            if (!const_cast<PluginManager*>(this)->unload_plugin(inspected.id, unload_err)) {
                error = unload_err.empty() ? "Failed to unload plugin before update" : unload_err;
                return false;
            }
        }
    }

    // Install transactionally with rollback on failure.
    PluginDescriptor out;
    std::string inst_err;
    bool ok = plugin_loader::install_plugin(filepath, out, inst_err);
    if (!ok) {
        error = inst_err;
        return false;
    }

    plugin_descriptor = out;

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(out.id);
        if (f) {
            bool was_loaded = f->is_loaded();
            bool was_restart = f->descriptor.restart_required;
            void* h = f->native_handle;
            void* ef = f->entry_fn;
            void* xf = f->exit_fn;
            bool jvm = f->jvm_loaded;
            f->descriptor = out;
            if (was_loaded) {
                f->native_handle = h;
                f->entry_fn = ef;
                f->exit_fn = xf;
                f->jvm_loaded = jvm;
                if (was_restart) f->descriptor.restart_required = true;
            }
            m_load_errors.erase(out.id);
        } else {
            LoadedPlugin np;
            np.descriptor = out;
            np.native_handle = nullptr;
            np.entry_fn = nullptr;
            np.exit_fn = nullptr;
            np.jvm_loaded = false;
            m_plugins.emplace_back(std::move(np));
            m_load_errors.erase(out.id);
        }
    }
    return true;
}

bool PluginManager::delete_plugin(const std::string& plugin_id, std::string& error)
{
    error.clear();
    if (!Plugin::Package::is_valid_plugin_id(plugin_id) || !Plugin::Package::is_safe_plugin_id_for_fs(plugin_id)) {
        error = "Invalid plugin id: " + plugin_id;
        return false;
    }

    PluginDescriptor desc;
    fs::path resolved_root;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        LoadedPlugin* f = find_plugin_locked(plugin_id);
        if (!f) { error = "Plugin not found: " + plugin_id; return false; }
        desc = f->descriptor;
    }

    {
        bool loaded = is_plugin_loaded(plugin_id);
        if (loaded) {
            std::string unload_err;
            if (!unload_plugin(plugin_id, unload_err)) {
                error = unload_err.empty() ? "Failed to unload plugin, restart required" : unload_err;
                return false;
            }
        }
    }

    std::vector<std::string> allowed = get_plugin_directories();
    std::string resolve_err;
    bool ok = resolve_allowed_plugin_root(desc, allowed, "Refusing to delete a plugin outside the known plugin directories.", resolved_root, resolve_err);
    if (!ok) {
        resolved_root = fs::path(get_orca_plugins_dir()) / plugin_id;
        if (!is_plugin_root_allowed(resolved_root, allowed)) {
            error = resolve_err;
            return false;
        }
    }

    std::string del_err;
    if (!delete_plugin_root(resolved_root, plugin_id, del_err)) {
        error = del_err;
        return false;
    }

    {
        std::lock_guard<std::mutex> lk(m_mutex);
        m_plugins.erase(std::remove_if(m_plugins.begin(), m_plugins.end(),
            [&](const LoadedPlugin& p){ return p.descriptor.id == plugin_id; }), m_plugins.end());
        m_load_errors.erase(plugin_id);
    }
    return true;
}

void PluginManager::open_plugin_folder(const std::string& plugin_id) const
{
    PluginDescriptor desc;
    if (!try_get_plugin_descriptor(plugin_id, desc)) return;

    fs::path root = resolve_plugin_root_from_descriptor(desc);
    if (root.empty()) root = fs::path(get_orca_plugins_dir()) / plugin_id;

    std::string path = root.string();

    boost::system::error_code ec;
    if (!fs::exists(root, ec)) return;

#ifdef _WIN32
    std::wstring w = boost::nowide::widen(path);
    ::ShellExecuteW(nullptr, L"open", w.c_str(), nullptr, nullptr, SW_SHOWNORMAL);
#else
    std::string cmd = "xdg-open \"" + path + "\" &";
    ::system(cmd.c_str());
#endif
}

bool PluginManager::set_plugin_error(const std::string& plugin_id, std::string error)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    LoadedPlugin* f = find_plugin_locked(plugin_id);
    if (!f) {
        m_load_errors[plugin_id] = error;
        return false;
    }
    f->descriptor.error = std::move(error);
    m_load_errors[plugin_id] = f->descriptor.error;
    return true;
}

bool PluginManager::clear_plugin_error(const std::string& plugin_id)
{
    std::lock_guard<std::mutex> lk(m_mutex);
    LoadedPlugin* f = find_plugin_locked(plugin_id);
    if (!f) {
        m_load_errors.erase(plugin_id);
        return false;
    }
    f->descriptor.error.clear();
    m_load_errors.erase(plugin_id);
    return true;
}

void PluginManager::subscribe_on_load_callback(PluginLifecycleCompleteFn fn)
{
    if (!fn) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_load_callbacks.push_back(std::move(fn));
}

void PluginManager::subscribe_on_unload_callback(PluginLifecycleCompleteFn fn)
{
    if (!fn) return;
    std::lock_guard<std::mutex> lk(m_mutex);
    m_unload_callbacks.push_back(std::move(fn));
}

std::string PluginManager::current_build_id() const
{
    return plugin_loader::current_build_id_string();
}

bool PluginManager::is_jvm_available() const
{
    if (Plugin::Jvm::is_jvm_off_build()) return false;
    return Plugin::Jvm::JvmPluginRuntime::instance().is_available();
}

std::string PluginManager::jvm_error() const
{
    std::lock_guard<std::mutex> lk(m_mutex);
    if (!m_jvm_error.empty()) return m_jvm_error;
    if (Plugin::Jvm::is_jvm_off_build()) return "JVM runtime not available (build without JVM support)";
    if (!Plugin::Jvm::JvmPluginRuntime::instance().is_available()) return "JVM not available or not initialized";
    return {};
}

void PluginManager::autoload_enabled_plugins()
{
    if (m_shutting_down.load()) return;
    std::vector<std::string> to_load;
    {
        std::lock_guard<std::mutex> lk(m_mutex);
        for (auto &p : m_plugins) {
            if (p.descriptor.enabled && !p.is_loaded() && !p.descriptor.has_error() && p.descriptor.metadata_valid) {
                to_load.push_back(p.descriptor.id);
            } else if (p.descriptor.enabled && p.descriptor.has_error()) {
                BOOST_LOG_TRIVIAL(warning) << "[orca-plugins] skip enabled plugin '" << p.descriptor.id << "' due to discovery error: " << p.descriptor.error;
                try { boost::nowide::cerr << "[orca-plugins] skip plugin '" << p.descriptor.id << "': " << p.descriptor.error << std::endl; } catch (...) {}
            } else if (p.descriptor.enabled && !p.descriptor.metadata_valid) {
                BOOST_LOG_TRIVIAL(warning) << "[orca-plugins] skip enabled plugin '" << p.descriptor.id << "' metadata invalid: " << p.descriptor.error;
                try { boost::nowide::cerr << "[orca-plugins] skip plugin '" << p.descriptor.id << "' metadata invalid: " << p.descriptor.error << std::endl; } catch (...) {}
            }
        }
        std::sort(to_load.begin(), to_load.end());
    }
    if (to_load.empty()) {
        BOOST_LOG_TRIVIAL(info) << "[orca-plugins] autoload: no enabled plugins to load (found " << m_plugins.size() << " total)";
        return;
    }
    BOOST_LOG_TRIVIAL(info) << "[orca-plugins] autoload: trying to load " << to_load.size() << " enabled plugin(s)";
    std::vector<std::string> native_first;
    std::vector<std::string> jvm_second;
    for (auto &id : to_load) {
        PluginDescriptor d;
        if (try_get_plugin_descriptor(id, d)) {
            if (d.runtime == "jvm") jvm_second.push_back(id);
            else native_first.push_back(id);
        }
    }
    auto do_load = [&](const std::string& id){
        std::string err;
        bool ok = load_plugin(id, err);
        if (ok) {
            BOOST_LOG_TRIVIAL(info) << "[orca-plugins] loaded '" << id << "'";
        } else {
            std::string detailed = err.empty() ? get_plugin_load_error(id) : err;
            if (detailed.empty()) detailed = "unknown error";
            BOOST_LOG_TRIVIAL(error) << "[orca-plugins] failed to load '" << id << "': " << detailed;
            try { boost::nowide::cerr << "[orca-plugins] failed to load '" << id << "': " << detailed << std::endl; } catch (...) {}
        }
        return ok;
    };
    for (auto &id : native_first) do_load(id);
    for (auto &id : jvm_second) do_load(id);
}

} // namespace Slic3r
