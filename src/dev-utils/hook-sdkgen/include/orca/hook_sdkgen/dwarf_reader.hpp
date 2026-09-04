#pragma once
#include "orca/hook_sdkgen/reader.hpp"
#include "orca/hook_sdkgen/model.hpp"
#include <string>

namespace orca::hook_sdkgen {

// Linux ELF/DWARF reader. Uses ElfImage for program headers, GNU build-id,
// .text hash, symtab/relocations, then traverses DWARF 2..5 via libdwarf
// 2.3.2 for types, subprograms, ranges and source lines.
// Mirrors Windows DiaReader canonical ids/signatures and typed/raw rules so
// the same C++/Rust/JVM SDK templates can consume either manifest.

class DwarfReader : public ISymbolReader {
public:
    Manifest read(const std::string& image_path, const std::string& debug_path) override;
    std::string name() const override { return "DwarfReader"; }

private:
    // Helpers exposed for testing
    void validate_elf_identity(const std::string& image_path, const std::string& debug_path, BuildInfo& out_build);
};

} // namespace orca::hook_sdkgen
