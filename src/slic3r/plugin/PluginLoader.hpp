#pragma once

#include "PluginDescriptor.hpp"

#include <boost/filesystem/path.hpp>

#include <string>

namespace Slic3r {
struct LoadedPlugin;
}

namespace Slic3r::plugin_loader {

bool is_allowed_artifact(const boost::filesystem::path &path, std::string &normalized_ext, std::string &error);

// Inspect without executing plugin code.
bool inspect_local_plugin_package(const boost::filesystem::path &filepath,
                                  PluginDescriptor& plugin_descriptor,
                                  bool& existing_installation,
                                  std::string& error);

// Install transactionally: inspect, copy into orca_plugins/<id>/, write InstallState.
bool install_plugin(const boost::filesystem::path &filepath,
                    PluginDescriptor& plugin_descriptor,
                    std::string& error);
bool install_plugin(const boost::filesystem::path &filepath, std::string& error);

// Load/unload a discovered plugin; never executes on build mismatch.
bool load_plugin(LoadedPlugin& plugin, std::string& error);
void unload_plugin(LoadedPlugin& plugin, std::string& error);
void unload_plugin(LoadedPlugin& plugin); // no error

// Current build id for mismatch checks (empty when the runtime is not initialized).
std::string current_build_id_string();
bool has_build_match(const PluginDescriptor& desc);

} // namespace Slic3r::plugin_loader
