#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <string>
#include <vector>
#include <array>
#include <cstdint>

namespace orca::hook_sdkgen {

// ELF64 / GNU build-id reader. No execution of the image; only reads file
// and DWARF section headers via raw parsing. libdwarf traversal is performed
// by DwarfReader which consumes the ElfInfo produced here.

struct ElfBuildId {
    std::vector<uint8_t> bytes; // raw NOTE payload (typically 20 bytes SHA1)
    std::string hex;            // lower hex
    bool found = false;
};

struct ElfSection {
    std::string name;
    uint32_t type = 0;
    uint64_t flags = 0;
    uint64_t virtual_address = 0;
    uint64_t file_offset = 0;
    uint64_t size = 0;
    uint32_t link = 0;
    uint32_t info = 0;
    uint64_t addralign = 0;
    uint64_t entsize = 0;
};

struct ElfSegment {
    uint32_t type = 0;
    uint32_t flags = 0;
    uint64_t offset = 0;
    uint64_t vaddr = 0;
    uint64_t filesz = 0;
    uint64_t memsz = 0;
    uint64_t align = 0;
};

struct ElfSymbol {
    std::string name;
    uint64_t value = 0;   // st_value (virtual address)
    uint64_t size = 0;    // st_size
    uint8_t bind = 0;     // STB_*
    uint8_t type = 0;     // STT_*
    uint16_t shndx = 0;
    std::string section_name;
    bool is_import = false; // UND
};

struct ElfRelocation {
    uint64_t offset = 0;  // r_offset (virtual address of GOT slot)
    uint32_t type = 0;    // R_X86_64_*
    uint32_t sym_index = 0;
    int64_t addend = 0;
    std::string sym_name;
    std::string section_name; // .rela.plt / .rela.dyn
};

struct ElfInfo {
    bool is_64 = false;
    bool is_little = false;
    uint16_t machine = 0;
    uint8_t elf_class = 0;
    uint64_t entry = 0;
    uint64_t image_base_vaddr = 0; // lowest PT_LOAD p_vaddr, used for RVA translation
    std::vector<ElfSegment> segments;
    std::vector<ElfSection> sections;
    ElfBuildId gnu_build_id;
    // Raw SHA256 of the file
    std::array<uint8_t,32> sha256{};
    // SHA256 of .text bytes only (for diagnostics)
    std::array<uint8_t,32> text_sha256{};
    uint64_t text_rva = 0;
    uint64_t text_size = 0;
    std::vector<uint8_t> text_bytes;
    std::vector<ElfSymbol> symbols;     // from .symtab + .dynsym (merged, dedup helpers)
    std::vector<ElfSymbol> dyn_symbols; // dynamic only
    std::vector<ElfRelocation> relocations; // .rela.plt + .rela.dyn
    std::string soname;
};

// Throws GenerationError on failure. Reads ELF64 LE file at elf_path.
ElfInfo read_elf_info(const std::string& elf_path);

// Translate virtual address to RVA (va - image_base_vaddr). For ET_DYN/ET_EXEC
// the manifest stores RVA from image base so runtime can do base+rva.
uint64_t va_to_rva(const ElfInfo& info, uint64_t va);
uint64_t rva_to_va(const ElfInfo& info, uint64_t rva);
std::string rva_to_section_name(const ElfInfo& info, uint64_t rva);

// Helpers used by DwarfReader and tests
std::array<uint8_t,32> sha256_file(const std::string& path);
std::array<uint8_t,32> sha256_bytes(const uint8_t* data, size_t len);

// Find GOT/PLT slot virtual address for module!symbol. Returns 0 if not found.
// module may be empty (match any) or soname substring case-insensitive.
uint64_t find_got_slot_va(const ElfInfo& info, const std::string& module, const std::string& symbol);

} // namespace orca::hook_sdkgen
