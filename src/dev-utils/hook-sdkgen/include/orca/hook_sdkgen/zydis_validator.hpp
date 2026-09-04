#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <string>
#include <vector>
#include <cstdint>

namespace orca::hook_sdkgen {

// Instruction boundary validation using Zydis if available, otherwise fallback.

struct ZydisStatus {
    bool available = false;
    std::string version;
    std::string reason; // if not available
};

// Returns whether Zydis headers were found at compile time.
ZydisStatus zydis_status();

// Validate instruction boundaries for a symbol's bytes.
// raw_bytes: bytes of symbol (size = symbol.size, starting at rva)
// Returns list of Instruction entries with offsets validated.
// If Zydis unavailable, returns empty and marks reason; caller should still emit symbol but without instr validation.
std::vector<Instruction> validate_instructions(const uint8_t* bytes, size_t len, uint32_t base_rva, std::string* error);

// Check if offset is at instruction boundary (for OFFSET hook validation). Requires previous validate.
// Returns true if offset is at start of an instruction.
bool is_instruction_boundary(const std::vector<Instruction>& instrs, uint32_t offset);

} // namespace orca::hook_sdkgen
