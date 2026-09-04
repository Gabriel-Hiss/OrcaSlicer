#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/dia_reader.hpp"
#include "orca/hook_sdkgen/dwarf_reader.hpp"

namespace orca::hook_sdkgen {

std::unique_ptr<ISymbolReader> create_dia_reader(){ return std::make_unique<DiaReader>(); }

std::unique_ptr<ISymbolReader> create_dwarf_reader(){ return std::make_unique<DwarfReader>(); }
std::unique_ptr<ISymbolReader> create_pe_image_reader(){ return nullptr; }

} // namespace
