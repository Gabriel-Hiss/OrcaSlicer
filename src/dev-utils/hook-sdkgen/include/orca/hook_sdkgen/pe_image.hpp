#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <string>
#include <vector>
#include <array>
#include <cstdint>

namespace orca::hook_sdkgen {

// PE parsing utilities: extracts CodeView GUID+age and computes image hash.
// No execution of image; only reads file.

struct PeCodeView {
    std::array<uint8_t,16> guid{};
    uint32_t age = 0;
    std::string pdb_file_name; // as stored in CodeView
    bool found = false;
};

struct PeInfo {
    PeCodeView codeview;
    uint32_t image_base = 0;
    uint32_t entry_rva = 0;
    uint32_t import_rva = 0;
    uint32_t import_size = 0;
    uint32_t debug_rva = 0;
    uint32_t debug_size = 0;
    struct Section {
        std::string name;
        uint32_t virtual_address = 0;
        uint32_t virtual_size = 0;
        uint32_t raw_offset = 0;
        uint32_t raw_size = 0;
        uint32_t characteristics = 0;
    };
    std::vector<Section> sections;
    // Raw image bytes hash
    std::array<uint8_t,32> sha256{};
};

// Reads PE file, validates DOS/NT headers, extracts CodeView and sections, computes SHA256 of file content.
// Throws GenerationError on failure.
PeInfo read_pe_info(const std::string& pe_path);

// Compute SHA256 of file (binary). Used for build_id.
std::array<uint8_t,32> sha256_file(const std::string& path);
std::array<uint8_t,32> sha256_bytes(const uint8_t* data, size_t len);

std::string rva_to_section_name(const PeInfo& info, uint32_t rva);

} // namespace orca::hook_sdkgen
