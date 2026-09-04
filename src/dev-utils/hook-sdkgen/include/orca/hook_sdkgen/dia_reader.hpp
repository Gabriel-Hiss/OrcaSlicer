#pragma once
#include "orca/hook_sdkgen/reader.hpp"
#ifdef ORCA_DIA_AVAILABLE
#include <string>
namespace orca::hook_sdkgen {

// Windows DIA-based reader. Requires VSINSTALLDIR/DIA SDK at build time
// and DIA runtime at generation time. Fails deterministically if PDB/GUID mismatch.

class DiaReader : public ISymbolReader {
public:
    Manifest read(const std::string& image_path, const std::string& debug_path) override;
    std::string name() const override { return "DiaReader"; }

private:
    void init_com();
    void check_pdb_match(const std::string& image_path, const std::string& pdb_path, BuildInfo& out_build);
};

} // namespace orca::hook_sdkgen

#else

// Non-Windows stub: DiaReader not available. Provides clean exclusion.
namespace orca::hook_sdkgen {
class DiaReader : public ISymbolReader {
public:
    Manifest read(const std::string&, const std::string&) override;
    std::string name() const override;
};
} // namespace orca::hook_sdkgen

#endif
