#pragma once

#include <string>
#include <vector>
#include <optional>
#include <nlohmann/json.hpp>

namespace Slic3r::Plugin::Package {
constexpr int PLUGIN_METADATA_SCHEMA_VERSION = 1;
constexpr int PLUGIN_HOOK_ABI_VERSION        = 1;
constexpr const char *PLUGIN_PE_RESOURCE_TYPE = "ORCA_PLUGIN_METADATA";
constexpr int PLUGIN_PE_RESOURCE_ID          = 1;
constexpr const char *PLUGIN_JAR_ENTRY       = "META-INF/orca/plugin.json";
constexpr const char *PLUGIN_ELF_NOTE_SECTION = ".note.orca.plugin";
constexpr const char *PLUGIN_ELF_NOTE_NAME   = "ORCA";

struct PluginTarget
{
    std::string os;       // windows | linux
    std::string arch;     // x86_64
    std::string build_id; // exact build identifier: <os>-<arch>-<guid>-<age>-<sha12> or platform-specific
};

struct PluginMetadata
{
    int schema = PLUGIN_METADATA_SCHEMA_VERSION;
    std::string id;       // [a-z0-9][a-z0-9._-]{2,127}
    std::string name;
    std::string version;  // semver
    std::string runtime;  // native | jvm
    std::string language; // cpp | rust | java | kotlin
    int hook_abi = PLUGIN_HOOK_ABI_VERSION;
    std::vector<PluginTarget> targets;
    std::optional<std::string> entry_class; // required when runtime == jvm
    std::optional<std::string> description;
    std::optional<std::string> author;
};

// Validation helpers (do not throw).
bool is_valid_plugin_id(const std::string &id);
bool is_valid_semver(const std::string &version, std::string *normalized = nullptr);
bool is_safe_filename(const std::string &filename);
bool is_safe_path_component(const std::string &component);

// Validate JSON object against schema 1. On failure returns false and fills error.
bool validate_plugin_metadata_json(const std::string &json_text, PluginMetadata &out, std::string &error);
bool validate_plugin_metadata_json(const nlohmann::json &j, PluginMetadata &out, std::string &error);

// Target / build matching: exact string equality on build_id.
bool has_exact_build_match(const PluginMetadata &meta, const std::string &current_build_id);
bool has_exact_build_match(const PluginMetadata &meta, const std::string &current_build_id, const PluginTarget *&matched);
bool is_runtime_language_compatible(const PluginMetadata &meta, std::string &error);

} // namespace Slic3r::Plugin::Package
