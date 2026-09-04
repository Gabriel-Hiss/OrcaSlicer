#pragma once

#include "HookDefs.hpp"
#include <cstdint>
#include <string>
#include <unordered_map>
#include <vector>

namespace Slic3r::Hook {

// Binary layout (LE, v1) produced by hook-sdkgen (slim runtime, 12.2MB):
//   magic 4B 'O' 'H' 'B' 'K' (0x4F 0x48 0x42 0x4B)
//   format_version u32 =1
//   hook_abi u32 =1
//   os u8, arch u8, reserved u16
//   image_sha256 32B
//   debug_guid 16B raw LE (Windows GUID) or 0 if Linux
//   debug_age u32 LE (Windows age) or 0
//   debug_file_len u32 LE + utf8 (pdb/elf path)
//   build_id_len u32 LE + utf8 (computed build_id string)
//   pool_flag u8 (0 raw, 1 zlib) ; if 1: decomp_size u32, comp_size u32, zlib bytes; if 0: raw pool follows
//     pool_raw: pool_size u32, then each w_string u32 len + utf8 (deduped, sorted)
//   symbol_count u32 LE
//   per symbol slim (18 bytes + ic*4):
//     pool_idx u32, hash u32 (fnv1a), rva u32, size u32, kind u8, pad u8, pad u16, ic u16 (0 in slim, OFFSET validated at generation/live via Zydis)
//   payload_sha256 32B footer (SHA256 of everything before footer)
// JSON manifest (author) is separate, gz 61.5MB, not used at runtime.
struct BuildId {
    uint8_t os = 0;   // 1=windows, 2=linux
    uint8_t arch = 1; // 1=x86_64
    uint8_t image_hash[32] = {};
    uint8_t debug_guid[16] = {}; // raw GUID (Windows) or 0
    uint32_t debug_age = 0;
    std::string gnu_build_id;    // hex (Linux)
    std::string debug_file;      // utf8 path
    bool operator==(const BuildId& o) const noexcept;
};

struct SymbolRange {
    uint32_t rva = 0;
    uint32_t size = 0;
};

struct TypedBinding {
    bool available = false;
    std::string reason;
};

struct ManifestSymbol {
    std::string id;
    std::string decorated;
    std::string readable;
    uint8_t kind = 0; // 0 func, 1 data, 2 vtable, etc.
    uint32_t type_index = 0;
    uint64_t rva = 0;
    uint32_t size = 0;
    TypedBinding typed;
    std::vector<SymbolRange> ranges;
    std::vector<uint32_t> instr_offsets;
};

struct ManifestType {
    uint32_t id = 0;
    uint8_t kind = 0;
    uint32_t size = 0;
    uint32_t align = 0;
    std::string name;
};

class HookManifest {
public:
    uint32_t format_version = 0;
    uint32_t hook_abi = 0;
    BuildId build{};
    std::vector<ManifestSymbol> symbols;
    std::vector<ManifestType> types;

    // Indexed lookup; reader is const after Load succeeds.
    bool LoadFromBinaryFile(const std::string& path, std::string& error);
    bool LoadFromBinary(const uint8_t* data, size_t size, std::string& error);

    // JSON loader for tests and tooling.
    bool LoadFromJsonFile(const std::string& path, std::string& error);
    bool LoadFromJson(const std::string& json, std::string& error);

    // The manifest build_id must exactly match the running image before any hook arms.
    bool ValidateBuildId(const BuildId& active, std::string& error) const;

    const ManifestSymbol* FindById(const std::string& id) const noexcept;
    const ManifestSymbol* FindByRva(uint64_t rva) const noexcept;
    const ManifestType* FindType(uint32_t type_index) const noexcept;

    // OFFSET requires an rva inside the symbol at a Zydis-validated instruction boundary.
    bool IsValidInstructionBoundary(const ManifestSymbol& sym, uint64_t target_rva) const noexcept;

    bool Empty() const noexcept { return symbols.empty(); }

    bool IsDeterministic() const noexcept; // symbols sorted by id, types by id

private:
    std::unordered_map<std::string, size_t> id_index_;
    std::unordered_map<uint64_t, size_t> rva_index_;
    std::unordered_map<uint32_t, size_t> type_index_;

    void RebuildIndices();
    static bool ComputePayloadHash(const uint8_t* data, size_t payload_size, uint8_t out[32]);
};

} // namespace Slic3r::Hook
