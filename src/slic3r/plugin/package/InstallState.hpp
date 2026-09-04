#pragma once

#include <string>
#include <boost/filesystem/path.hpp>

namespace Slic3r::Plugin::Package {

constexpr int INSTALL_STATE_SCHEMA_VERSION = 1;
constexpr const char *INSTALL_STATE_FILENAME = ".install_state.json";

// Persisted per installed plugin directory (data_dir/orca_plugins/<id>/).
struct InstallState
{
    int schema = INSTALL_STATE_SCHEMA_VERSION;
    std::string artifact; // base filename only, e.g. "myplugin.dll"
    std::string hash;     // hex SHA-256 (64 chars, lowercase)
    std::string version;  // semver string (from metadata)
    bool enabled = true;
};

bool write_install_state(const boost::filesystem::path &plugin_dir, const InstallState &state, std::string &error);
bool read_install_state(const boost::filesystem::path &plugin_dir, InstallState &out, std::string &error);
bool read_install_state_file(const boost::filesystem::path &file, InstallState &out, std::string &error);
bool write_install_state_file(const boost::filesystem::path &file, const InstallState &state, std::string &error);

} // namespace Slic3r::Plugin::Package
