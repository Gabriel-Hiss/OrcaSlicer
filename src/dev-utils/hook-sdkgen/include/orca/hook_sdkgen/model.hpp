#pragma once
#include <cstdint>
#include <string>
#include <vector>
#include <array>
#include <optional>
#include <map>

namespace orca::hook_sdkgen {

constexpr uint32_t kFormatVersion = 1;
constexpr uint32_t kHookAbiVersion = 1;

enum class Os : uint8_t {
    Unknown = 0,
    Windows = 1,
    Linux = 2,
};

enum class Arch : uint8_t {
    Unknown = 0,
    X86_64 = 1,
};

inline std::string os_to_string(Os os) {
    switch (os) {
        case Os::Windows: return "windows";
        case Os::Linux: return "linux";
        default: return "unknown";
    }
}
inline std::string arch_to_string(Arch arch) {
    switch (arch) {
        case Arch::X86_64: return "x86_64";
        default: return "unknown";
    }
}

struct BuildInfo {
    Os os = Os::Unknown;
    Arch arch = Arch::Unknown;
    std::string debug_file; // PDB file name or DWARF file
    std::array<uint8_t, 16> debug_guid{}; // CodeView GUID raw bytes
    uint32_t debug_age = 0;
    std::array<uint8_t, 32> image_sha256{}; // SHA-256 of PE/ELF image
    std::string image_path; // original image path for reporting
    std::string build_id; // computed: <os>-<arch>-<guid>-<age>-<sha256hex[0:12]>

    std::string debug_guid_string() const; // 8-4-4-4-12 lower hex
    std::string image_sha256_hex() const;
    void recompute_build_id();
};

enum class TypeKind : uint8_t {
    Void = 0,
    Bool,
    I8, U8, I16, U16, I32, U32, I64, U64,
    F32, F64,
    Enum,
    Pointer,
    Reference,
    Array,
    Struct,
    Class,
    Union,
    Function,
    Typedef,
    Unknown,
};

std::string type_kind_to_string(TypeKind k);

struct TypeInfo {
    uint32_t id = 0;
    TypeKind kind = TypeKind::Unknown;
    std::string name;
    uint32_t size = 0;
    uint32_t align = 0;
    // For enums
    std::map<int64_t, std::string> enum_values;
    // For aggregates
    struct Field {
        std::string name;
        uint32_t type_id = 0;
        uint32_t offset = 0;
    };
    std::vector<Field> fields;
    // For pointers/arrays
    uint32_t pointee_type = 0;
    uint32_t array_count = 0;
    // For functions
    uint32_t return_type = 0;
    std::vector<uint32_t> param_types;
    bool is_trivially_copyable = false;
    bool is_complete = true;
};

struct TypedBinding {
    bool available = false;
    std::string reason; // empty if available
};

enum class SymbolKind : uint8_t {
    Function = 0,
    Data = 1,
    VTable = 2,
    Import = 3,
    Unknown = 255,
};

std::string symbol_kind_to_string(SymbolKind k);
SymbolKind symbol_kind_from_string(const std::string& s);

struct Range {
    uint32_t rva = 0;
    uint32_t size = 0;
    bool operator<(const Range& o) const {
        if (rva != o.rva) return rva < o.rva;
        return size < o.size;
    }
};

struct Instruction {
    uint32_t offset = 0; // offset from symbol rva
    uint8_t size = 0;
    bool is_call = false;
    bool is_ret = false;
};

struct SourceLocation {
    std::string file;
    uint32_t line = 0;
    uint32_t column = 0;
};

struct Symbol {
    std::string id; // stable id, e.g. decorated name or hash
    std::string decorated_name;
    std::string display_name;
    SymbolKind kind = SymbolKind::Unknown;
    uint64_t rva = 0; // RVA from image base (32-bit on PE, 64-bit on ELF)
    uint32_t size = 0;
    uint32_t type_id = 0; // 0 = no type
    std::string section;
    std::string calling_convention; // e.g. __cdecl, __stdcall, x64, sysv
    std::vector<Range> ranges;
    std::vector<Instruction> instructions;
    std::optional<SourceLocation> source;
    TypedBinding typed_binding;

    // Deterministic sort key: id
    bool operator<(const Symbol& o) const { return id < o.id; }
};

struct GenerationStats {
    uint32_t total_symbols = 0;
    uint32_t typed_available = 0;
    uint32_t raw_only = 0;
    uint32_t inlined_or_no_address = 0;
    uint32_t functions = 0;
    uint32_t data = 0;
    uint32_t vtables = 0;
    uint32_t imports = 0;
};

struct Manifest {
    uint32_t format_version = kFormatVersion;
    uint32_t hook_abi = kHookAbiVersion;
    BuildInfo build;
    std::vector<TypeInfo> types; // deduped, sorted by id
    std::vector<Symbol> symbols; // sorted by id
    GenerationStats stats;

    void sort_deterministic();
    void recompute_stats();
    bool validate(std::string* err) const;
};

std::string format_guid(const std::array<uint8_t,16>& guid);
std::string hex_bytes(const uint8_t* data, size_t len);
std::string hex_bytes(const std::array<uint8_t,32>& data);

} // namespace orca::hook_sdkgen
