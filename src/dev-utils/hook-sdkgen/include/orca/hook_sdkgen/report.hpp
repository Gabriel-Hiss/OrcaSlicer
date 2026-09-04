#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <string>

namespace orca::hook_sdkgen {

struct GenerationReport {
    std::string image_path;
    std::string pdb_path;
    BuildInfo build;
    GenerationStats stats;
    bool success = false;
    std::string error;
    std::string zydis_status;
    std::string dia_status;
    std::string manifest_path;
    std::string runtime_path;
};

void write_report(const GenerationReport& report, const std::string& path);
GenerationReport make_report(const Manifest* manifest, const std::string& image_path, const std::string& pdb_path, const std::string& manifest_path, const std::string& runtime_path, const std::string& error);

} // namespace
