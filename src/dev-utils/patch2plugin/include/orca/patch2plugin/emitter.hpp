#pragma once
#include "orca/patch2plugin/manifest.hpp"
#include "orca/patch2plugin/brace_scanner.hpp"
#include <map>
#include <string>
#include <vector>

namespace orca::patch2plugin {

struct ConvertedFunc {
    std::string file; // normalized relative
    int line = 0;
    std::string func_name;
    std::string qualified;
    std::string symbol_id;
    uint64_t rva = 0;
    std::string full_text;
};

struct RejectedChunk {
    std::string file;
    int line = 0;
    std::string reason;
};

void emit_plugin_project(
    const std::string& out_dir,
    const std::string& plugin_id,
    const std::string& version,
    const std::string& plugin_name,
    const ManifestInfo& manifest,
    const std::vector<ConvertedFunc>& converted,
    const std::map<std::string, std::string>& file_contents_post // file -> post content
);

std::string sanitize_symbol(const std::string& s);
std::string json_escape2(const std::string& s);

} // namespace orca::patch2plugin
