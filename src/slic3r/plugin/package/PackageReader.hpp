#pragma once

#include "PluginMetadata.hpp"

#include <string>
#include <boost/filesystem/path.hpp>

namespace Slic3r::Plugin::Package {

// Inspection never executes plugin code.

// PE file: read resource ORCA_PLUGIN_METADATA id 1 without LoadLibrary.
bool read_pe_metadata(const boost::filesystem::path &dll_path, std::string &json_text, std::string &error);
bool read_pe_metadata(const boost::filesystem::path &dll_path, PluginMetadata &out, std::string &error);

// ELF file: read .note.orca.plugin note section.
bool read_elf_metadata(const boost::filesystem::path &so_path, std::string &json_text, std::string &error);
bool read_elf_metadata(const boost::filesystem::path &so_path, PluginMetadata &out, std::string &error);

// JAR file: read META-INF/orca/plugin.json without classloading.
bool read_jar_metadata(const boost::filesystem::path &jar_path, std::string &json_text, std::string &error);
bool read_jar_metadata(const boost::filesystem::path &jar_path, PluginMetadata &out, std::string &error);

bool read_plugin_metadata_file(const boost::filesystem::path &path, std::string &json_text, std::string &error);
bool read_plugin_metadata_file(const boost::filesystem::path &path, PluginMetadata &out, std::string &error);

// Inspect a file, validate metadata, and compute its SHA-256 without executing it.
struct InspectResult
{
    bool ok = false;
    PluginMetadata metadata;
    std::string json_text;     // raw metadata json bytes
    std::string artifact_hash; // hex SHA-256 of entire file
    std::string error;         // populated when !ok
};

InspectResult inspect_plugin_file(const boost::filesystem::path &path);
InspectResult inspect_plugin_file(const boost::filesystem::path &path, std::string &error_out);

// Validate id/path before filesystem copy.
bool is_safe_plugin_id_for_fs(const std::string &id);
bool is_allowed_artifact_extension(const boost::filesystem::path &path, std::string &normalized_ext);

} // namespace Slic3r::Plugin::Package
