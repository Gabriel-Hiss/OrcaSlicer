#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <memory>
#include <string>
#include <stdexcept>
#include <functional>

namespace orca::hook_sdkgen {

// Abstract symbol reader. Implementations: DiaReader (Windows PDB), DwarfReader (Linux).
class ISymbolReader {
public:
    virtual ~ISymbolReader() = default;
    // Read symbols from image + debug info. Throws on failure (mismatch etc).
    // image_path: PE/ELF file, debug_path: PDB/DWARF file (may be same as image on Linux).
    virtual Manifest read(const std::string& image_path, const std::string& debug_path) = 0;
    virtual std::string name() const = 0;
};

#ifdef _WIN32
// DiaReader is only available on Windows with DIA SDK.
std::unique_ptr<ISymbolReader> create_dia_reader();
#endif
std::unique_ptr<ISymbolReader> create_dwarf_reader();
std::unique_ptr<ISymbolReader> create_pe_image_reader(); // reads only PE identity, not symbols

struct GenerationError : std::runtime_error {
    using std::runtime_error::runtime_error;
};

} // namespace orca::hook_sdkgen
