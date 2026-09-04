#include "PluginFsUtils.hpp"

#include "slic3r/plugin/package/Hash.hpp"
#include "slic3r/plugin/package/InstallState.hpp"
#include "slic3r/plugin/package/PackageReader.hpp"
#include "slic3r/plugin/package/PluginMetadata.hpp"

#include "libslic3r/Utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>

#include <algorithm>
#include <cctype>
#include <fstream>

namespace Slic3r {

const char* const INSTALL_STATE_FILE = ".install_state.json";

std::string get_orca_plugins_dir()
{
    return (boost::filesystem::path(data_dir()) / "orca_plugins").string();
}

std::vector<std::string> get_plugin_directories()
{
    std::string dir = get_orca_plugins_dir();
    boost::filesystem::create_directories(dir);
    return {dir};
}

std::vector<std::string> get_plugin_directories(const std::string&)
{
    return get_plugin_directories();
}

boost::filesystem::path resolve_plugin_root_from_descriptor(const PluginDescriptor& descriptor)
{
    if (!descriptor.plugin_root.empty())
        return boost::filesystem::path(descriptor.plugin_root);
    if (!descriptor.artifact_path.empty())
        return boost::filesystem::path(descriptor.artifact_path).parent_path();
    if (!descriptor.artifact_path.empty())
        return boost::filesystem::path(descriptor.artifact_path).parent_path();
    return {};
}

bool is_plugin_root_allowed(const boost::filesystem::path& candidate_root,
                            const std::vector<std::string>& allowed_dirs)
{
    boost::system::error_code ec;
    boost::filesystem::path resolved = boost::filesystem::weakly_canonical(candidate_root, ec);
    if (ec) {
        ec.clear();
        resolved = boost::filesystem::absolute(candidate_root, ec);
    }
    if (ec || resolved.empty()) return false;
    for (auto &d : allowed_dirs) {
        boost::filesystem::path allowed = boost::filesystem::absolute(d, ec);
        if (ec) continue;
        std::string r = resolved.string();
        std::string a = allowed.string();
        if (r == a) return true;
#ifdef _WIN32
        std::string prefix = a + '\\';
#else
        std::string prefix = a + '/';
#endif
        if (r.size() > prefix.size() && r.compare(0, prefix.size(), prefix) == 0) return true;
        // Compare case-insensitively on Windows.
#ifdef _WIN32
        std::string rl = r, al = a;
        std::transform(rl.begin(), rl.end(), rl.begin(), ::tolower);
        std::transform(al.begin(), al.end(), al.begin(), ::tolower);
        if (rl == al) return true;
        std::string pl = prefix;
        std::transform(pl.begin(), pl.end(), pl.begin(), ::tolower);
        if (rl.size() > pl.size() && rl.compare(0, pl.size(), pl) == 0) return true;
#endif
    }
    return false;
}

bool resolve_allowed_plugin_root(const PluginDescriptor& descriptor,
                                 const std::vector<std::string>& allowed_dirs,
                                 const std::string& out_of_scope_error,
                                 boost::filesystem::path& resolved_root,
                                 std::string& error)
{
    boost::filesystem::path root = resolve_plugin_root_from_descriptor(descriptor);
    if (root.empty()) {
        error = "Plugin folder could not be determined.";
        return false;
    }
    boost::system::error_code ec;
    resolved_root = boost::filesystem::weakly_canonical(root, ec);
    if (ec) {
        ec.clear();
        resolved_root = boost::filesystem::absolute(root, ec);
    }
    if (ec || resolved_root.empty()) {
        error = "Failed to resolve plugin folder: " + root.string();
        return false;
    }
    if (!is_plugin_root_allowed(resolved_root, allowed_dirs)) {
        error = out_of_scope_error.empty() ? "Plugin folder is outside allowed directories." : out_of_scope_error;
        return false;
    }
    return true;
}

bool delete_plugin_root(const boost::filesystem::path& resolved_root,
                        const std::string& plugin_id,
                        std::string& error)
{
    if (!is_valid_plugin_id(plugin_id)) {
        error = "Invalid plugin id: " + plugin_id;
        return false;
    }
    // The folder name must match the plugin id.
    if (resolved_root.filename().string() != plugin_id) {
        error = "Plugin folder name mismatch: " + resolved_root.string();
        return false;
    }
    boost::system::error_code ec;
    boost::filesystem::remove_all(resolved_root, ec);
    if (ec) {
        error = "Failed to delete plugin folder: " + ec.message();
        return false;
    }
    return true;
}

bool is_ignored_plugin_directory(const boost::filesystem::path& path)
{
    std::string name = path.filename().string();
    if (name.empty()) return true;
    if (name[0] == '.') return true;
    if (name == "__pycache__") return true;
    return false;
}

bool is_safe_relative_path(const boost::filesystem::path& path)
{
    if (path.is_absolute()) return false;
    for (auto &part : path) {
        std::string s = part.string();
        if (s == "..") return false;
        if (s.find('\0') != std::string::npos) return false;
    }
    return true;
}

bool is_valid_plugin_id(const std::string& id)
{
    return Plugin::Package::is_valid_plugin_id(id);
}

bool is_allowed_artifact_extension(const boost::filesystem::path& path, std::string& normalized_ext)
{
    normalized_ext = path.extension().string();
    std::transform(normalized_ext.begin(), normalized_ext.end(), normalized_ext.begin(), ::tolower);
    if (normalized_ext == ".dll" || normalized_ext == ".so" || normalized_ext == ".jar") return true;
    return false;
}

boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error)
{
    error.clear();
    boost::system::error_code ec;
    if (!boost::filesystem::exists(plugin_dir, ec) || !boost::filesystem::is_directory(plugin_dir, ec)) {
        error = "Plugin directory does not exist: " + plugin_dir.string();
        return {};
    }
    std::vector<boost::filesystem::path> candidates;
    for (boost::filesystem::directory_iterator it(plugin_dir, ec), end; it != end && !ec; it.increment(ec)) {
        if (ec) break;
        if (!boost::filesystem::is_regular_file(it->path(), ec)) continue;
        std::string ext = it->path().extension().string();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
        if (ext == ".dll" || ext == ".so" || ext == ".jar") {
            if (it->path().filename().string() == INSTALL_STATE_FILE) continue;
            candidates.push_back(it->path());
        }
    }
    if (candidates.empty()) {
        error = "No plugin artifact (.dll/.so/.jar) in " + plugin_dir.string();
        return {};
    }
    if (candidates.size() > 1) {
        error = "Multiple artifacts in " + plugin_dir.string();
        return {};
    }
    return candidates.front();
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry)
{
    return write_install_state(plugin_dir, entry, entry.enabled);
}

bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry, bool enabled)
{
    Plugin::Package::InstallState st;
    st.schema = Plugin::Package::INSTALL_STATE_SCHEMA_VERSION;
    // Store the artifact base filename only.
    boost::filesystem::path art(entry.artifact_path.empty() ? entry.artifact_path : entry.artifact_path);
    st.artifact = art.filename().string();
    if (st.artifact.empty()) {
        std::string e;
        auto p = find_installed_plugin_entry(plugin_dir, e);
        if (!p.empty()) st.artifact = p.filename().string();
    }
    st.hash = entry.artifact_hash;
    st.version = entry.version;
    st.enabled = enabled;
    std::string err;
    return Plugin::Package::write_install_state(plugin_dir, st, err);
}

bool read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry)
{
    Plugin::Package::InstallState st;
    std::string err;
    if (!Plugin::Package::read_install_state(plugin_dir, st, err)) return false;
    entry.enabled = st.enabled;
    entry.install_state_valid = true;
    if (entry.artifact_path.empty() && !st.artifact.empty()) {
        entry.artifact_path = (plugin_dir / st.artifact).string();
        entry.artifact_path = entry.artifact_path;
    }
    if (entry.artifact_hash.empty()) entry.artifact_hash = st.hash;
    if (entry.version.empty()) entry.version = st.version;
    return true;
}

bool read_install_state(const boost::filesystem::path& plugin_dir, PluginInstallStateLegacy& out)
{
    Plugin::Package::InstallState st;
    std::string err;
    if (!Plugin::Package::read_install_state(plugin_dir, st, err)) return false;
    out.enabled = st.enabled;
    out.installed_version = st.version;
    return true;
}

std::vector<PluginDescriptor> discover_plugin_packages(const std::vector<std::string>& dirs, std::string& error)
{
    error.clear();
    std::vector<PluginDescriptor> out;
    for (auto &dir_str : dirs) {
        boost::filesystem::path dir(dir_str);
        boost::system::error_code ec;
        if (!boost::filesystem::exists(dir, ec)) continue;
        if (!boost::filesystem::is_directory(dir, ec)) continue;
        for (boost::filesystem::directory_iterator it(dir, ec), end; it != end && !ec; it.increment(ec)) {
            if (ec) break;
            boost::filesystem::path plugin_dir = it->path();
            if (!boost::filesystem::is_directory(plugin_dir, ec)) continue;
            if (is_ignored_plugin_directory(plugin_dir)) continue;
            std::string plugin_id = plugin_dir.filename().string();
            if (!is_valid_plugin_id(plugin_id)) {
                BOOST_LOG_TRIVIAL(warning) << "Ignoring invalid plugin dir name: " << plugin_dir;
                continue;
            }
            PluginDescriptor desc;
            desc.id = plugin_id;
            desc.plugin_root = plugin_dir.string();

            Plugin::Package::InstallState ist;
            std::string ist_err;
            bool has_ist = Plugin::Package::read_install_state(plugin_dir, ist, ist_err);
            if (has_ist) {
                desc.enabled = ist.enabled;
                desc.install_state_valid = true;
                desc.artifact_hash = ist.hash;
                if (desc.version.empty()) desc.version = ist.version;
            } else {
                // Without install state the plugin is never auto-loaded.
                desc.enabled = false;
                desc.install_state_valid = false;
            }

            std::string find_err;
            boost::filesystem::path art = find_installed_plugin_entry(plugin_dir, find_err);
            if (art.empty()) {
                desc.metadata_valid = false;
                desc.error = find_err;
                out.push_back(std::move(desc));
                continue;
            }
            desc.artifact_path = art.string();

            // Reject artifacts whose hash differs from the install state.
            bool hash_mismatch = false;
            std::string mismatch_msg;
            try {
                std::string hash_err;
                std::string computed = Plugin::Package::sha256_file_hex(art, hash_err);
                if (!hash_err.empty()) computed.clear();
                if (!computed.empty()) {
                    if (has_ist && !ist.hash.empty() && computed != ist.hash) {
                        hash_mismatch = true;
                        mismatch_msg = "Hash mismatch for " + plugin_id + " stored " + ist.hash + " computed " + computed;
                        BOOST_LOG_TRIVIAL(warning) << mismatch_msg;
                    }
                    desc.artifact_hash = computed;
                }
                if (hash_mismatch) {
                    desc.metadata_valid = false;
                    desc.error = mismatch_msg;
                    out.push_back(std::move(desc));
                    continue;
                }
            } catch (...) {}

            // Inspect metadata without executing code.
            Plugin::Package::InspectResult ir = Plugin::Package::inspect_plugin_file(art);
            if (!ir.ok) {
                desc.metadata_valid = false;
                desc.error = ir.error;
                out.push_back(std::move(desc));
                continue;
            }
            desc.name = ir.metadata.name;
            desc.version = ir.metadata.version;
            if (ir.metadata.description) desc.description = *ir.metadata.description;
            if (ir.metadata.author) desc.author = *ir.metadata.author;
            desc.runtime = ir.metadata.runtime;
            desc.language = ir.metadata.language;
            desc.hook_abi = ir.metadata.hook_abi;
            desc.targets = ir.metadata.targets;
            desc.entry_class = ir.metadata.entry_class;
            desc.id = ir.metadata.id;
            // The folder id must match the metadata id.
            if (plugin_id != ir.metadata.id) {
                desc.metadata_valid = false;
                desc.error = "Folder id '" + plugin_id + "' mismatches metadata id '" + ir.metadata.id + "'";
                out.push_back(std::move(desc));
                continue;
            }
            desc.artifact_hash = ir.artifact_hash;
            desc.metadata_valid = true;
            desc.clear_error();

            out.push_back(std::move(desc));
        }
    }
    return out;
}

} // namespace Slic3r
