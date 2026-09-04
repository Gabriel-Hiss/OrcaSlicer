#pragma once
#include <string>
#include <vector>
#include <cstdint>

namespace orca::patch2plugin {

struct ManifestSymbol {
    std::string id;
    std::string name; // decorated or display, fallback to id
    std::string display_name;
    uint64_t rva = 0;
    uint32_t size = 0;
    std::string source_file; // normalized with '/' and lower for compare
    uint32_t source_line = 0;
};

struct ManifestInfo {
    std::string build_id;
    std::string os;
    std::string arch;
    std::vector<ManifestSymbol> symbols;
};

// Load manifest from path (plain json or .gz). Throws on failure with message.
ManifestInfo load_manifest(const std::string& path);

std::string normalize_path(const std::string& p);
std::string basename_of(const std::string& p);
std::string json_escape(const std::string& s);

} // namespace orca::patch2plugin
