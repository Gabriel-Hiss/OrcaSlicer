#include "PluginLoader.hpp"
#include "PluginManager.hpp"
#include "PluginFsUtils.hpp"

#include "slic3r/plugin/package/PackageReader.hpp"
#include "slic3r/plugin/package/Hash.hpp"
#include "slic3r/plugin/package/InstallState.hpp"
#include "slic3r/plugin/hook/HookRuntime.hpp"
#include "slic3r/plugin/runtime/jvm/JvmPluginRuntime.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#ifdef _WIN32
#include <windows.h>
#else
#include <dlfcn.h>
#endif

#include <algorithm>
#include <cctype>
#include <chrono>
#include <thread>

namespace Slic3r::plugin_loader {

static std::string to_lower_str(std::string s) {
    std::transform(s.begin(), s.end(), s.begin(), [](unsigned char c){ return std::tolower(c); });
    return s;
}

bool is_allowed_artifact(const boost::filesystem::path &path, std::string &normalized_ext, std::string &error)
{
    normalized_ext = to_lower_str(path.extension().string());
    if (normalized_ext == ".dll" || normalized_ext == ".so" || normalized_ext == ".jar") return true;
    error = "Unsupported artifact extension (expected .dll/.so/.jar): " + path.string();
    return false;
}

// Build-id string helpers matching hook-sdkgen.
static std::string hex_bytes_lower(const uint8_t* data, size_t len) {
    static const char* hex = "0123456789abcdef";
    std::string out; out.reserve(len*2);
    for (size_t i=0;i<len;++i){ out.push_back(hex[(data[i]>>4)&0xF]); out.push_back(hex[data[i]&0xF]); }
    return out;
}

static std::string guid_to_string(const uint8_t guid[16]) {
    // PE GUID fields Data1/Data2/Data3 are little-endian; format canonical 8-4-4-4-12.
    char buf[37];
    std::snprintf(buf, sizeof(buf), "%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
        guid[3], guid[2], guid[1], guid[0],
        guid[5], guid[4],
        guid[7], guid[6],
        guid[8], guid[9],
        guid[10], guid[11], guid[12], guid[13], guid[14], guid[15]);
    std::string s(buf);
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string current_build_id_string()
{
    using namespace Slic3r::Hook;
    if (!HookRuntime::Instance().IsInitialized()) return {};
    BuildId bid = HookRuntime::Instance().ActiveBuildId();
    // Return empty when the backend reports no build id.
    bool empty = true;
    for (auto b: bid.image_hash) if (b!=0) empty=false;
    for (auto b: bid.debug_guid) if (b!=0) empty=false;
    if (!bid.gnu_build_id.empty()) empty=false;
    if (empty && bid.debug_age==0) return {};

    std::string os_str = (bid.os==1 ? "windows" : (bid.os==2 ? "linux" :
#ifdef _WIN32
                         "windows"
#else
                         "linux"
#endif
                         ));
    std::string sha_hex = hex_bytes_lower(bid.image_hash, 32);
    std::string short_sha = sha_hex.substr(0,12);
    if (!bid.gnu_build_id.empty()) {
        std::string g = bid.gnu_build_id;
        g = to_lower_str(g);
        std::string short_g = g.substr(0, std::min<size_t>(16, g.size()));
        if (short_g.size()<16) short_g = std::string(16 - short_g.size(),'0') + short_g;
        return "linux-x86_64-" + short_g + "-" + short_sha;
    }
    std::string guid = guid_to_string(bid.debug_guid);
    return os_str + "-x86_64-" + guid + "-" + std::to_string(bid.debug_age) + "-" + short_sha;
}

bool has_build_match(const PluginDescriptor& desc)
{
    std::string cur = current_build_id_string();
    if (cur.empty()) return false;
    Plugin::Package::PluginMetadata tmp;
    tmp.targets = desc.targets;
    return Plugin::Package::has_exact_build_match(tmp, cur);
}

bool inspect_local_plugin_package(const boost::filesystem::path &filepath,
                                  PluginDescriptor& plugin_descriptor,
                                  bool& existing_installation,
                                  std::string& error)
{
    error.clear();
    plugin_descriptor = PluginDescriptor{};
    existing_installation = false;

    std::string ext;
    if (!is_allowed_artifact(filepath, ext, error)) return false;

    boost::system::error_code ec;
    if (!boost::filesystem::exists(filepath, ec) || ec) {
        error = "File does not exist: " + filepath.string();
        return false;
    }

    Plugin::Package::InspectResult ir = Plugin::Package::inspect_plugin_file(filepath);
    if (!ir.ok) {
        error = ir.error;
        return false;
    }
    plugin_descriptor.id = ir.metadata.id;
    plugin_descriptor.id = ir.metadata.id;
    plugin_descriptor.name = ir.metadata.name;
    plugin_descriptor.version = ir.metadata.version;
    if (ir.metadata.description) plugin_descriptor.description = *ir.metadata.description;
    if (ir.metadata.author) plugin_descriptor.author = *ir.metadata.author;
    plugin_descriptor.runtime = ir.metadata.runtime;
    plugin_descriptor.language = ir.metadata.language;
    plugin_descriptor.hook_abi = ir.metadata.hook_abi;
    plugin_descriptor.targets = ir.metadata.targets;
    plugin_descriptor.entry_class = ir.metadata.entry_class;
    plugin_descriptor.artifact_hash = ir.artifact_hash;
    plugin_descriptor.metadata_valid = true;
    plugin_descriptor.artifact_path = filepath.string();
    plugin_descriptor.artifact_path = plugin_descriptor.artifact_path;

    boost::filesystem::path installed_dir = boost::filesystem::path(get_orca_plugins_dir()) / plugin_descriptor.id;
    if (boost::filesystem::exists(installed_dir, ec) && boost::filesystem::is_directory(installed_dir, ec)) {
        existing_installation = true;
        std::string e;
        auto entry = find_installed_plugin_entry(installed_dir, e);
        if (!entry.empty()) {}
    }
    return true;
}

static bool transactional_copy_install(const boost::filesystem::path &src, const std::string &plugin_id, PluginDescriptor& out_desc, std::string &error)
{
    namespace fs = boost::filesystem;
    std::string ext = to_lower_str(src.extension().string());
    std::string orca_dir = get_orca_plugins_dir();
    fs::create_directories(orca_dir);
    fs::path dest_dir = fs::path(orca_dir) / plugin_id;
    fs::path dest_file = dest_dir / (plugin_id + ext);
    dest_file = dest_dir / src.filename();

    Plugin::Package::InspectResult ir = Plugin::Package::inspect_plugin_file(src);
    if (!ir.ok) { error = ir.error; return false; }
    std::string hash = ir.artifact_hash;

    boost::system::error_code ec;
    fs::create_directories(dest_dir, ec);
    if (ec) { error = "Failed to create plugin dir: " + ec.message(); return false; }

    // Back up the existing artifact and install state.
    fs::path backup_dir;
    bool had_existing = fs::exists(dest_dir, ec) && fs::is_directory(dest_dir, ec);
    fs::path tmp_dir = fs::path(orca_dir) / (plugin_id + ".tmp");
    fs::remove_all(tmp_dir, ec);
    fs::create_directories(tmp_dir, ec);

    std::string find_err;
    fs::path existing_art = find_installed_plugin_entry(dest_dir, find_err);
    fs::path backup_art;
    fs::path backup_state = tmp_dir / ".install_state.json.bak";
    fs::path state_file = dest_dir / Plugin::Package::INSTALL_STATE_FILENAME;

    if (!existing_art.empty() && fs::exists(existing_art, ec)) {
        backup_art = tmp_dir / existing_art.filename();
        fs::copy_file(existing_art, backup_art, fs::copy_option::overwrite_if_exists, ec);
        if (ec) { error = "Backup failed: " + ec.message(); return false; }
        if (fs::exists(state_file, ec)) {
            fs::copy_file(state_file, backup_state, fs::copy_option::overwrite_if_exists, ec);
        }
    }

    // Copy via temp dir so a failed install leaves the previous artifact intact.
    fs::path tmp_file = tmp_dir / src.filename();
    fs::copy_file(src, tmp_file, fs::copy_option::overwrite_if_exists, ec);
    if (ec) { error = "Copy failed: " + ec.message(); return false; }

    if (!existing_art.empty() && fs::exists(existing_art, ec)) {
        fs::remove(existing_art, ec);
    }
    fs::copy_file(tmp_file, dest_file, fs::copy_option::overwrite_if_exists, ec);
    if (ec) { error = "Install copy failed: " + ec.message(); goto rollback; }

    {
        PluginDescriptor tmp_desc;
        tmp_desc.id = ir.metadata.id;
        tmp_desc.version = ir.metadata.version;
        tmp_desc.artifact_path = dest_file.string();
        tmp_desc.artifact_hash = hash;
        tmp_desc.enabled = true;
        if (!write_install_state(dest_dir, tmp_desc)) {
            error = "Failed to write install state";
            goto rollback;
        }
        out_desc = PluginDescriptor{};
        out_desc.id = ir.metadata.id;
        out_desc.name = ir.metadata.name;
        out_desc.version = ir.metadata.version;
        if (ir.metadata.description) out_desc.description = *ir.metadata.description;
        if (ir.metadata.author) out_desc.author = *ir.metadata.author;
        out_desc.runtime = ir.metadata.runtime;
        out_desc.language = ir.metadata.language;
        out_desc.hook_abi = ir.metadata.hook_abi;
        out_desc.targets = ir.metadata.targets;
        out_desc.entry_class = ir.metadata.entry_class;
        out_desc.plugin_root = dest_dir.string();
        out_desc.artifact_path = dest_file.string();
        out_desc.artifact_hash = hash;
        out_desc.enabled = true;
        out_desc.install_state_valid = true;
        out_desc.metadata_valid = true;
    }

    fs::remove_all(tmp_dir, ec);
    return true;

rollback:
    if (!backup_art.empty() && fs::exists(backup_art, ec)) {
        fs::copy_file(backup_art, existing_art, fs::copy_option::overwrite_if_exists, ec);
    }
    if (fs::exists(backup_state, ec)) {
        fs::copy_file(backup_state, state_file, fs::copy_option::overwrite_if_exists, ec);
    } else {
        fs::remove(state_file, ec);
    }
    fs::remove_all(tmp_dir, ec);
    if (!had_existing) {
        fs::remove_all(dest_dir, ec);
    }
    return false;
}

bool install_plugin(const boost::filesystem::path &filepath, PluginDescriptor& plugin_descriptor, std::string& error)
{
    error.clear();
    // Refuse overwrite while a restart is pending.
    plugin_descriptor = PluginDescriptor{};
    bool existing = false;
    PluginDescriptor tmp;
    std::string insp_err;
    if (!inspect_local_plugin_package(filepath, tmp, existing, insp_err)) {
        error = insp_err;
        return false;
    }
    try {
        Slic3r::PluginManager &mgr = Slic3r::PluginManager::instance();
        PluginDescriptor existing_desc;
        if (mgr.try_get_plugin_descriptor(tmp.id, existing_desc) && existing_desc.restart_required) {
            error = "Plugin '" + tmp.id + "' requires restart before it can be updated. Restart the application and try again.";
            return false;
        }
    } catch (...) {}

    return transactional_copy_install(filepath, tmp.id, plugin_descriptor, error);
}

bool install_plugin(const boost::filesystem::path &filepath, std::string& error)
{
    PluginDescriptor dummy;
    return install_plugin(filepath, dummy, error);
}

#ifdef _WIN32
static void* native_load(const std::string& path, std::string& error) {
    std::wstring wpath = boost::nowide::widen(path);
    HMODULE h = ::LoadLibraryW(wpath.c_str());
    if (!h) {
        DWORD err = ::GetLastError();
        error = "LoadLibrary failed (" + std::to_string(err) + "): " + path;
        return nullptr;
    }
    return (void*)h;
}
static void native_unload(void* h) { if (h) ::FreeLibrary((HMODULE)h); }
static void* native_symbol(void* h, const char* name) { return (void*)::GetProcAddress((HMODULE)h, name); }
#else
static void* native_load(const std::string& path, std::string& error) {
    void* h = ::dlopen(path.c_str(), RTLD_NOW | RTLD_LOCAL);
    if (!h) { error = std::string("dlopen failed: ") + ::dlerror(); return nullptr; }
    return h;
}
static void native_unload(void* h) { if (h) ::dlclose(h); }
static void* native_symbol(void* h, const char* name) { return ::dlsym(h, name); }
#endif

bool load_plugin(LoadedPlugin& plugin, std::string& error)
{
    error.clear();
    if (plugin.descriptor.restart_required) {
        error = "Plugin '" + plugin.descriptor.id + "' requires restart.";
        return false;
    }
    if (!plugin.descriptor.metadata_valid) {
        error = "Plugin metadata invalid: " + plugin.descriptor.error;
        return false;
    }
    // Refuse to execute on build mismatch.
    if (!has_build_match(plugin.descriptor)) {
        std::string cur = current_build_id_string();
        error = "Build mismatch: plugin requires build_id in [";
        for (size_t i=0;i<plugin.descriptor.targets.size();++i){
            if (i) error += ", ";
            error += plugin.descriptor.targets[i].build_id;
        }
        error += "] but current is '" + cur + "'";
        plugin.descriptor.error = error;
        return false;
    }
    if (plugin.descriptor.runtime == "jvm") {
        bool is_off = Slic3r::Plugin::Jvm::is_jvm_off_build();
        if (is_off) {
            error = "JVM runtime not available (build without JVM support)";
            plugin.descriptor.error = error;
            return false;
        }
        // JVM plugins require an entry class.
        if (!plugin.descriptor.entry_class || plugin.descriptor.entry_class->empty()) {
            error = "JVM plugin missing entry_class";
            plugin.descriptor.error = error;
            return false;
        }
        auto &jvm = Slic3r::Plugin::Jvm::JvmPluginRuntime::instance();
        std::string jerr;
        const orca_host_api_v1_t* host = Slic3r::Hook::HookRuntime::Instance().GetHostApi();
        if (!jvm.ensure_vm(host, jerr)) {
            error = "JVM not available: " + jerr;
            plugin.descriptor.error = error;
            return false;
        }
        // Plugin scoping lives in JvmHostBridge, not host TLS.
        if (!jvm.load_jar(plugin.descriptor.id, plugin.descriptor.artifact_path, *plugin.descriptor.entry_class, jerr)) {
            error = jerr;
            plugin.descriptor.error = error;
            return false;
        }
        plugin.jvm_loaded = true;
        plugin.native_handle = nullptr; // sentinel for is_loaded
        plugin.descriptor.error.clear();
        plugin.descriptor.restart_required = false;
        return true;
    } else if (plugin.descriptor.runtime == "native") {
        std::string load_err;
        void* handle = native_load(plugin.descriptor.artifact_path, load_err);
        if (!handle) { error = load_err; plugin.descriptor.error = error; return false; }
        using entry_t = int(*)(const orca_host_api_v1_t*);
        using exit_t = void(*)();
        entry_t entry = (entry_t)native_symbol(handle, "orca_plugin_entry_v1");
        exit_t  ex = (exit_t)native_symbol(handle, "orca_plugin_exit_v1");
        if (!entry) {
            native_unload(handle);
            error = "Missing export orca_plugin_entry_v1 in " + plugin.descriptor.artifact_path;
            plugin.descriptor.error = error;
            return false;
        }
        Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi(plugin.descriptor.id);
        const orca_host_api_v1_t* host = Slic3r::Hook::HookRuntime::Instance().GetHostApi();
        int rc = 0;
        try {
            rc = entry(host);
        } catch (const std::exception& exn) {
            Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi({});
            native_unload(handle);
            error = std::string("Plugin entry threw: ") + exn.what();
            plugin.descriptor.error = error;
            // Remove hooks installed before the throw.
            std::string rem_err;
            Slic3r::Hook::HookRuntime::Instance().RemovePluginHooks(plugin.descriptor.id, 0, rem_err);
            return false;
        } catch (...) {
            Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi({});
            native_unload(handle);
            error = "Plugin entry threw unknown exception";
            plugin.descriptor.error = error;
            std::string rem_err;
            Slic3r::Hook::HookRuntime::Instance().RemovePluginHooks(plugin.descriptor.id, 0, rem_err);
            return false;
        }
        Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi({});
        if (rc != 0) {
            native_unload(handle);
            // Remove leftover hooks from the failed entry.
            std::string rem_err;
            Slic3r::Hook::HookRuntime::Instance().RemovePluginHooks(plugin.descriptor.id, 0, rem_err);
            std::string lastHook = Slic3r::Hook::HookRuntime::GetLastHookError();
            const char* statusName = Slic3r::Hook::HookRuntime::HookStatusName((orca_hook_status_t)rc);
            if (!lastHook.empty()) {
                error = "Plugin '" + plugin.descriptor.id + "' hook install failed: " + lastHook;
            } else {
                error = "Plugin '" + plugin.descriptor.id + "' entry returned error code " + std::to_string(rc) + " (" + statusName + ")";
                std::string residual = Slic3r::Hook::HookRuntime::GetLastHookError();
                if (!residual.empty()) error += ": " + residual;
            }
            plugin.descriptor.error = error;
            Slic3r::Hook::HookRuntime::SetLastHookError({});
            return false;
        }
        plugin.native_handle = handle;
        plugin.entry_fn = (void*)entry;
        plugin.exit_fn = (void*)ex;
        plugin.jvm_loaded = false;
        plugin.descriptor.error.clear();
        plugin.descriptor.restart_required = false;
        return true;
    } else {
        error = "Unknown runtime: " + plugin.descriptor.runtime;
        plugin.descriptor.error = error;
        return false;
    }
}

void unload_plugin(LoadedPlugin& plugin, std::string& error)
{
    error.clear();
    // Drain hooks before releasing the module; keep it resident when drain needs a restart.
    std::string rem_err;
    auto status = Slic3r::Hook::HookRuntime::Instance().RemovePluginHooks(plugin.descriptor.id, Slic3r::Hook::kDefaultDrainTimeoutMs, rem_err);
    if (status == Slic3r::Hook::HookStatus::RestartRequired) {
        plugin.descriptor.restart_required = true;
        error = "Unload requires restart (active callbacks did not drain)";
        return;
    } else if (status != Slic3r::Hook::HookStatus::Ok && status != Slic3r::Hook::HookStatus::NotFound) {
        BOOST_LOG_TRIVIAL(warning) << "RemovePluginHooks failed: " << rem_err;
    }

    if (plugin.descriptor.runtime == "jvm" && plugin.jvm_loaded) {
        auto &jvm = Slic3r::Plugin::Jvm::JvmPluginRuntime::instance();
        std::string jerr;
        jvm.unload_jar(plugin.descriptor.id, jerr);
        if (!jerr.empty()) BOOST_LOG_TRIVIAL(warning) << "Jvm unload error: " << jerr;
        plugin.jvm_loaded = false;
        plugin.native_handle = nullptr;
        plugin.descriptor.restart_required = false;
        return;
    }
    if (plugin.native_handle) {
        if (plugin.exit_fn) {
            Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi(plugin.descriptor.id);
            try {
                auto fn = (void(*)())plugin.exit_fn;
                fn();
            } catch (...) {
                BOOST_LOG_TRIVIAL(warning) << "Plugin exit threw for " << plugin.descriptor.id;
            }
            Slic3r::Hook::HookRuntime::Instance().SetCurrentPluginForAbi({});
        }
        if (!plugin.descriptor.restart_required) {
            native_unload(plugin.native_handle);
            plugin.native_handle = nullptr;
        } else {
        }
        plugin.entry_fn = nullptr;
        plugin.exit_fn = nullptr;
    }
}

void unload_plugin(LoadedPlugin& plugin)
{
    std::string e;
    unload_plugin(plugin, e);
}

} // namespace Slic3r::plugin_loader
