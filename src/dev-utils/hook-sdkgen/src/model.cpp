#include "orca/hook_sdkgen/model.hpp"
#include <algorithm>
#include <sstream>
#include <iomanip>
#include <cctype>

namespace orca::hook_sdkgen {

std::string type_kind_to_string(TypeKind k) {
    switch (k) {
        case TypeKind::Void: return "void";
        case TypeKind::Bool: return "bool";
        case TypeKind::I8: return "i8";
        case TypeKind::U8: return "u8";
        case TypeKind::I16: return "i16";
        case TypeKind::U16: return "u16";
        case TypeKind::I32: return "i32";
        case TypeKind::U32: return "u32";
        case TypeKind::I64: return "i64";
        case TypeKind::U64: return "u64";
        case TypeKind::F32: return "f32";
        case TypeKind::F64: return "f64";
        case TypeKind::Enum: return "enum";
        case TypeKind::Pointer: return "pointer";
        case TypeKind::Reference: return "reference";
        case TypeKind::Array: return "array";
        case TypeKind::Struct: return "struct";
        case TypeKind::Class: return "class";
        case TypeKind::Union: return "union";
        case TypeKind::Function: return "function";
        case TypeKind::Typedef: return "typedef";
        default: return "unknown";
    }
}

std::string symbol_kind_to_string(SymbolKind k) {
    switch (k) {
        case SymbolKind::Function: return "function";
        case SymbolKind::Data: return "data";
        case SymbolKind::VTable: return "vtable";
        case SymbolKind::Import: return "import";
        default: return "unknown";
    }
}
SymbolKind symbol_kind_from_string(const std::string& s) {
    if (s == "function") return SymbolKind::Function;
    if (s == "data") return SymbolKind::Data;
    if (s == "vtable") return SymbolKind::VTable;
    if (s == "import") return SymbolKind::Import;
    return SymbolKind::Unknown;
}

std::string hex_bytes(const uint8_t* data, size_t len) {
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (size_t i = 0; i < len; ++i) oss << std::setw(2) << static_cast<int>(data[i]);
    return oss.str();
}
std::string hex_bytes(const std::array<uint8_t,32>& data) { return hex_bytes(data.data(), data.size()); }

std::string format_guid(const std::array<uint8_t,16>& guid) {
    // PE CodeView stores GUID as struct GUID with Data1 LE, Data2 LE, Data3 LE, Data4 BE.
    // Data1: bytes 0-3 LE, Data2: 4-5 LE, Data3: 6-7 LE, Data4: 8-15 BE.
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    oss << std::setw(2) << (int)guid[3] << std::setw(2) << (int)guid[2] << std::setw(2) << (int)guid[1] << std::setw(2) << (int)guid[0] << "-";
    oss << std::setw(2) << (int)guid[5] << std::setw(2) << (int)guid[4] << "-";
    oss << std::setw(2) << (int)guid[7] << std::setw(2) << (int)guid[6] << "-";
    oss << std::setw(2) << (int)guid[8] << std::setw(2) << (int)guid[9] << "-";
    for (int i = 10; i < 16; ++i) oss << std::setw(2) << (int)guid[i];
    std::string s = oss.str();
    for (char &c : s) c = (char)std::tolower((unsigned char)c);
    return s;
}

std::string BuildInfo::debug_guid_string() const { return format_guid(debug_guid); }
std::string BuildInfo::image_sha256_hex() const { return hex_bytes(image_sha256); }

void BuildInfo::recompute_build_id() {
    std::string guid = debug_guid_string();
    std::string hash = image_sha256_hex();
    std::string short_hash = hash.substr(0, 12);
    // age in decimal to avoid hex ambiguity
    build_id = os_to_string(os) + "-" + arch_to_string(arch) + "-" + guid + "-" + std::to_string(debug_age) + "-" + short_hash;
}

void Manifest::sort_deterministic() {
    std::sort(types.begin(), types.end(), [](const TypeInfo& a, const TypeInfo& b){ return a.id < b.id; });
    // also sort fields inside types for determinism
    for (auto& t : types) {
        std::sort(t.fields.begin(), t.fields.end(), [](const auto& a, const auto& b){ return a.name < b.name; });
        // enum_values is map, already sorted
    }
    std::sort(symbols.begin(), symbols.end(), [](const Symbol& a, const Symbol& b){
        if (a.id != b.id) return a.id < b.id;
        return a.rva < b.rva;
    });
    for (auto& s : symbols) {
        std::sort(s.ranges.begin(), s.ranges.end());
        std::sort(s.instructions.begin(), s.instructions.end(), [](const Instruction& a, const Instruction& b){ return a.offset < b.offset; });
    }
}

void Manifest::recompute_stats() {
    stats = {};
    stats.total_symbols = (uint32_t)symbols.size();
    for (auto& s : symbols) {
        if (s.typed_binding.available) stats.typed_available++; else stats.raw_only++;
        switch (s.kind) {
            case SymbolKind::Function: stats.functions++; break;
            case SymbolKind::Data: stats.data++; break;
            case SymbolKind::VTable: stats.vtables++; break;
            case SymbolKind::Import: stats.imports++; break;
            default: break;
        }
    }
    // inlined_or_no_address is set by reader; keep if already set, otherwise 0.
}

bool Manifest::validate(std::string* err) const {
    if (format_version != kFormatVersion) {
        if (err) *err = "format_version mismatch";
        return false;
    }
    if (hook_abi != kHookAbiVersion) {
        if (err) *err = "hook_abi mismatch";
        return false;
    }
    if (build.build_id.empty()) {
        if (err) *err = "build_id empty";
        return false;
    }
    for (size_t i = 1; i < types.size(); ++i) if (types[i-1].id >= types[i].id) { if (err) *err = "types not sorted"; return false; }
    for (size_t i = 1; i < symbols.size(); ++i) if (symbols[i-1].id > symbols[i].id) { if (err) *err = "symbols not sorted"; return false; }
    return true;
}

} // namespace orca::hook_sdkgen
