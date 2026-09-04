#pragma once

#include <algorithm>
#include <optional>
#include <string>
#include <vector>

#include "slic3r/plugin/package/PluginMetadata.hpp"

namespace Slic3r {

struct PluginDescriptor
{
    std::string id;               // [a-z0-9][a-z0-9._-]{2,127}
    std::string name;
    std::string version;          // semver
    std::string description;
    std::string author;

    std::string runtime;  // native | jvm
    std::string language; // cpp | rust | java | kotlin
    int hook_abi = 1;

    std::vector<Plugin::Package::PluginTarget> targets;
    std::optional<std::string> entry_class; // required when runtime == jvm

    std::string plugin_root;    // data_dir/orca_plugins/<id>
    std::string artifact_path;  // full path to .dll/.so/.jar
    std::string artifact_hash;  // hex SHA-256

    bool enabled = true;
    bool install_state_valid = false;
    bool metadata_valid = false;

    std::string error;
    bool restart_required = false;

    bool has_error() const { return !normalized_error().empty(); }
    bool is_invalid_package() const { return !metadata_valid; }

    std::string normalized_error() const
    {
        auto begin = std::find_if_not(error.begin(), error.end(), [](unsigned char ch) { return std::isspace(ch) != 0; });
        if (begin == error.end()) return {};
        auto end = std::find_if_not(error.rbegin(), error.rend(), [](unsigned char ch) { return std::isspace(ch) != 0; }).base();
        return std::string(begin, end);
    }

    bool is_metadata_valid() const { return metadata_valid; }

    void clear_error() { error.clear(); }
    void set_error(std::string msg) { error = std::move(msg); }
};

} // namespace Slic3r
