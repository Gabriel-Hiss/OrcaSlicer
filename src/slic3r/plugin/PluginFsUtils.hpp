#pragma once

#include "PluginDescriptor.hpp"

#include <string>
#include <vector>

#include <boost/filesystem/path.hpp>

#define PLUGIN_DATA_DIR "plugin_data"

namespace Slic3r {

extern const char* const INSTALL_STATE_FILE;

std::string get_orca_plugins_dir();
boost::filesystem::path resolve_plugin_root_from_descriptor(const PluginDescriptor& descriptor);

bool is_plugin_root_allowed(const boost::filesystem::path& candidate_root,
                            const std::vector<std::string>& allowed_dirs);

bool resolve_allowed_plugin_root(const PluginDescriptor& descriptor,
                                 const std::vector<std::string>& allowed_dirs,
                                 const std::string& out_of_scope_error,
                                 boost::filesystem::path& resolved_root,
                                 std::string& error);

bool delete_plugin_root(const boost::filesystem::path& resolved_root,
                        const std::string& plugin_id,
                        std::string& error);

std::vector<std::string> get_plugin_directories();
std::vector<std::string> get_plugin_directories(const std::string& /*ignored*/);

// Discover plugins without executing code.
std::vector<PluginDescriptor> discover_plugin_packages(const std::vector<std::string>& dirs, std::string& error);

bool is_ignored_plugin_directory(const boost::filesystem::path& path);
bool is_safe_relative_path(const boost::filesystem::path& path);
bool is_valid_plugin_id(const std::string& id);
bool is_allowed_artifact_extension(const boost::filesystem::path& path, std::string& normalized_ext);

// Locate the single installed artifact in a plugin dir.
boost::filesystem::path find_installed_plugin_entry(const boost::filesystem::path& plugin_dir, std::string& error);

// InstallState read/write keeps the single-field schema.
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry);
bool write_install_state(const boost::filesystem::path& plugin_dir, const PluginDescriptor& entry, bool enabled);
bool read_install_state(const boost::filesystem::path& plugin_dir, PluginDescriptor& entry);
bool read_install_state(const boost::filesystem::path& plugin_dir, struct PluginInstallStateLegacy& out);

struct PluginInstallStateLegacy {
    bool enabled = true;
    std::string installed_version;
};

} // namespace Slic3r
