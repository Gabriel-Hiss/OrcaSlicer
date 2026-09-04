#include "PackageReader.hpp"
#include "Hash.hpp"
#include "PluginMetadata.hpp"

#include <algorithm>
#include <cctype>
#include <cstring>
#include <fstream>
#include <vector>
#include <limits>

#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include "miniz.h"

namespace Slic3r::Plugin::Package {

namespace {

// Parse PE resources without LoadLibrary so inspection never executes code.

#pragma pack(push, 1)
struct DosHeader {
    uint16_t e_magic;     // MZ
    uint16_t e_cblp;
    uint16_t e_cp;
    uint16_t e_crlc;
    uint16_t e_cparhdr;
    uint16_t e_minalloc;
    uint16_t e_maxalloc;
    uint16_t e_ss;
    uint16_t e_sp;
    uint16_t e_csum;
    uint16_t e_ip;
    uint16_t e_cs;
    uint16_t e_lfarlc;
    uint16_t e_ovno;
    uint16_t e_res[4];
    uint16_t e_oemid;
    uint16_t e_oeminfo;
    uint16_t e_res2[10];
    int32_t  e_lfanew;
};
struct PeFileHeader {
    uint32_t Signature; // PE\0\0
    uint16_t Machine;
    uint16_t NumberOfSections;
    uint32_t TimeDateStamp;
    uint32_t PointerToSymbolTable;
    uint32_t NumberOfSymbols;
    uint16_t SizeOfOptionalHeader;
    uint16_t Characteristics;
};
struct DataDirectory {
    uint32_t VirtualAddress;
    uint32_t Size;
};
struct OptionalHeader64 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint64_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint64_t SizeOfStackReserve;
    uint64_t SizeOfStackCommit;
    uint64_t SizeOfHeapReserve;
    uint64_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
};
struct OptionalHeader32 {
    uint16_t Magic;
    uint8_t  MajorLinkerVersion;
    uint8_t  MinorLinkerVersion;
    uint32_t SizeOfCode;
    uint32_t SizeOfInitializedData;
    uint32_t SizeOfUninitializedData;
    uint32_t AddressOfEntryPoint;
    uint32_t BaseOfCode;
    uint32_t BaseOfData;
    uint32_t ImageBase;
    uint32_t SectionAlignment;
    uint32_t FileAlignment;
    uint16_t MajorOperatingSystemVersion;
    uint16_t MinorOperatingSystemVersion;
    uint16_t MajorImageVersion;
    uint16_t MinorImageVersion;
    uint16_t MajorSubsystemVersion;
    uint16_t MinorSubsystemVersion;
    uint32_t Win32VersionValue;
    uint32_t SizeOfImage;
    uint32_t SizeOfHeaders;
    uint32_t CheckSum;
    uint16_t Subsystem;
    uint16_t DllCharacteristics;
    uint32_t SizeOfStackReserve;
    uint32_t SizeOfStackCommit;
    uint32_t SizeOfHeapReserve;
    uint32_t SizeOfHeapCommit;
    uint32_t LoaderFlags;
    uint32_t NumberOfRvaAndSizes;
};
struct SectionHeader {
    char     Name[8];
    uint32_t VirtualSize;
    uint32_t VirtualAddress;
    uint32_t SizeOfRawData;
    uint32_t PointerToRawData;
    uint32_t PointerToRelocations;
    uint32_t PointerToLinenumbers;
    uint16_t NumberOfRelocations;
    uint16_t NumberOfLinenumbers;
    uint32_t Characteristics;
};
struct ResDirHeader {
    uint32_t Characteristics;
    uint32_t TimeDateStamp;
    uint16_t MajorVersion;
    uint16_t MinorVersion;
    uint16_t NumberOfNamedEntries;
    uint16_t NumberOfIdEntries;
};
struct ResDirEntry {
    uint32_t NameOrId; // high bit = string, otherwise id
    uint32_t OffsetToData; // high bit = directory
};
struct ResDataEntry {
    uint32_t OffsetToData; // RVA
    uint32_t Size;
    uint32_t CodePage;
    uint32_t Reserved;
};
#pragma pack(pop)

static bool rva_to_file_offset(uint32_t rva, const std::vector<SectionHeader> &sections, uint32_t &out_offset)
{
    for (auto &sec : sections) {
        uint32_t va = sec.VirtualAddress;
        uint32_t vs = sec.VirtualSize;
        // Map RVA via VirtualSize for range and PointerToRawData for file offset.
        uint32_t end = va + (vs ? vs : sec.SizeOfRawData);
        if (rva >= va && rva < end) {
            uint32_t delta = rva - va;
            if (delta >= sec.SizeOfRawData) {
                // RVA beyond raw data (e.g., BSS) – no file bytes.
                return false;
            }
            out_offset = sec.PointerToRawData + delta;
            return true;
        }
    }
    return false;
}

static bool read_file_bytes(const boost::filesystem::path &path, std::vector<unsigned char> &out, std::string &error, size_t max_size = 256u * 1024u * 1024u)
{
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec) || !boost::filesystem::is_regular_file(path, ec)) {
        error = "file not found or not regular file: " + path.string();
        return false;
    }
    auto sz = boost::filesystem::file_size(path, ec);
    if (!ec && sz > max_size) {
        error = "file too large: " + path.string();
        return false;
    }
    boost::nowide::ifstream f(path.string(), std::ios::binary);
    if (!f) { error = "cannot open file: " + path.string(); return false; }
    out.assign(std::istreambuf_iterator<char>(f), std::istreambuf_iterator<char>());
    if (f.bad()) { error = "read error: " + path.string(); return false; }
    return true;
}


static bool extract_pe_resource_bytes(const std::vector<unsigned char> &image,
                                      std::vector<unsigned char> &res_bytes,
                                      std::string &error)
{
    if (image.size() < sizeof(DosHeader)) { error = "PE: file too small for DOS header"; return false; }
    DosHeader dos{};
    memcpy(&dos, image.data(), sizeof(dos));
    if (dos.e_magic != 0x5A4D) { // MZ
        error = "PE: missing MZ magic";
        return false;
    }
    if (dos.e_lfanew < 0 || (size_t)dos.e_lfanew + sizeof(PeFileHeader) > image.size()) {
        error = "PE: invalid e_lfanew";
        return false;
    }
    PeFileHeader fh{};
    memcpy(&fh, image.data() + dos.e_lfanew, sizeof(fh));
    if (fh.Signature != 0x00004550) { error = "PE: missing PE signature"; return false; }
    if (fh.NumberOfSections == 0 || fh.NumberOfSections > 96) { error = "PE: invalid NumberOfSections"; return false; }
    if (fh.SizeOfOptionalHeader > 1024) { error = "PE: invalid SizeOfOptionalHeader"; return false; }

    size_t opt_off = dos.e_lfanew + sizeof(PeFileHeader);
    if (opt_off + fh.SizeOfOptionalHeader > image.size()) { error = "PE: optional header out of range"; return false; }
    uint16_t magic = 0;
    memcpy(&magic, image.data() + opt_off, sizeof(magic));
    bool is_pe32plus = false;
    uint32_t num_rva_sizes = 0;
    std::vector<DataDirectory> dirs;
    size_t sec_off = 0;
    if (magic == 0x10b) {
        if (fh.SizeOfOptionalHeader < sizeof(OptionalHeader32)) { error = "PE: optional header32 too small"; return false; }
        OptionalHeader32 oh{};
        memcpy(&oh, image.data() + opt_off, sizeof(oh));
        num_rva_sizes = oh.NumberOfRvaAndSizes;
        if (num_rva_sizes > 16) num_rva_sizes = 16;
        size_t dir_off = opt_off + sizeof(oh);
        // The directory array follows the fixed optional-header part.
        if (dir_off + num_rva_sizes * sizeof(DataDirectory) > opt_off + fh.SizeOfOptionalHeader) {
            error = "PE: data directory out of range";
            return false;
        }
        dirs.resize(num_rva_sizes);
        for (uint32_t i = 0; i < num_rva_sizes; ++i) memcpy(&dirs[i], image.data() + dir_off + i * sizeof(DataDirectory), sizeof(DataDirectory));
        sec_off = opt_off + fh.SizeOfOptionalHeader;
        is_pe32plus = false;
    } else if (magic == 0x20b) {
        if (fh.SizeOfOptionalHeader < sizeof(OptionalHeader64)) { error = "PE: optional header64 too small"; return false; }
        OptionalHeader64 oh{};
        memcpy(&oh, image.data() + opt_off, sizeof(oh));
        num_rva_sizes = oh.NumberOfRvaAndSizes;
        if (num_rva_sizes > 16) num_rva_sizes = 16;
        size_t dir_off = opt_off + sizeof(oh);
        if (dir_off + num_rva_sizes * sizeof(DataDirectory) > opt_off + fh.SizeOfOptionalHeader) {
            error = "PE: data directory out of range";
            return false;
        }
        dirs.resize(num_rva_sizes);
        for (uint32_t i = 0; i < num_rva_sizes; ++i) memcpy(&dirs[i], image.data() + dir_off + i * sizeof(DataDirectory), sizeof(DataDirectory));
        sec_off = opt_off + fh.SizeOfOptionalHeader;
        is_pe32plus = true;
    } else {
        error = "PE: unknown optional header magic";
        return false;
    }
    (void)is_pe32plus;
    if (dirs.size() <= 2 || dirs[2].VirtualAddress == 0 || dirs[2].Size == 0) {
        error = "PE: no resource directory";
        return false;
    }
    uint32_t res_rva = dirs[2].VirtualAddress;
    uint32_t res_size = dirs[2].Size;

    if (sec_off + fh.NumberOfSections * sizeof(SectionHeader) > image.size()) {
        error = "PE: section headers out of range";
        return false;
    }
    std::vector<SectionHeader> sections(fh.NumberOfSections);
    for (int i = 0; i < fh.NumberOfSections; ++i) memcpy(&sections[i], image.data() + sec_off + i * sizeof(SectionHeader), sizeof(SectionHeader));

    uint32_t res_offset = 0;
    if (!rva_to_file_offset(res_rva, sections, res_offset)) {
        error = "PE: resource RVA not mapped to file offset";
        return false;
    }
    if ((size_t)res_offset + res_size > image.size()) {
        error = "PE: resource directory extends beyond file";
        return false;
    }
    auto read_u32_le = [&](size_t off, uint32_t &v)->bool{
        if (off + 4 > image.size()) return false;
        memcpy(&v, image.data()+off, 4);
        return true;
    };
    auto read_u16_le = [&](size_t off, uint16_t &v)->bool{
        if (off + 2 > image.size()) return false;
        memcpy(&v, image.data()+off, 2);
        return true;
    };
    auto get_resource_string = [&](uint32_t name_offset, std::string &out_str)->bool{
        // name_offset is offset from res base, lower 31 bits, points to: uint16 len + WCHAR[len]
        if (name_offset & 0x80000000u) {} // already masked by caller
        uint32_t off = name_offset & 0x7FFFFFFFu;
        size_t abs = (size_t)res_offset + (size_t)off;
        if (abs + 2 > image.size()) return false;
        uint16_t len = 0;
        memcpy(&len, image.data()+abs, 2);
        if (len > 256) return false;
        if (abs + 2 + (size_t)len*2 > (size_t)res_offset + res_size) return false;
        if (abs + 2 + (size_t)len*2 > image.size()) return false;
        out_str.clear();
        out_str.reserve(len);
        for (uint16_t i=0;i<len;++i){
            uint16_t wc=0;
            memcpy(&wc, image.data()+abs+2+i*2, 2);
            if (wc >= 0x80) {
                // Non-ASCII names cannot match ORCA_PLUGIN_METADATA.
                out_str.push_back('?');
            } else {
                out_str.push_back((char)wc);
            }
        }
        return true;
    };

    // Resource tree levels: Type, Name, Language.
    struct StackNode { uint32_t dir_offset; int level; };
    auto parse_dir = [&](uint32_t dir_rel_off, std::vector<ResDirEntry> &entries, ResDirHeader &hdr)->bool{
        size_t abs = (size_t)res_offset + (size_t)dir_rel_off;
        if (abs + sizeof(ResDirHeader) > image.size()) return false;
        if (abs + sizeof(ResDirHeader) > (size_t)res_offset + res_size) return false;
        memcpy(&hdr, image.data()+abs, sizeof(hdr));
        if (hdr.NumberOfNamedEntries > 64 || hdr.NumberOfIdEntries > 64) return false;
        size_t total = (size_t)hdr.NumberOfNamedEntries + hdr.NumberOfIdEntries;
        if (abs + sizeof(ResDirHeader) + total * sizeof(ResDirEntry) > (size_t)res_offset + res_size) return false;
        entries.resize(total);
        for (size_t i=0;i<total;++i) memcpy(&entries[i], image.data()+abs+sizeof(ResDirHeader)+i*sizeof(ResDirEntry), sizeof(ResDirEntry));
        return true;
    };

    ResDirHeader root_hdr{};
    std::vector<ResDirEntry> root_entries;
    if (!parse_dir(0, root_entries, root_hdr)) { error = "PE: cannot parse root resource directory"; return false; }

    uint32_t type_dir_off = 0;
    bool found_type = false;
    for (auto &e : root_entries) {
        bool is_named = (e.NameOrId & 0x80000000u) != 0;
        std::string type_str;
        uint32_t id = e.NameOrId & 0x7FFFFFFFu;
        if (is_named) {
            if (!get_resource_string(e.NameOrId, type_str)) continue;
            // RC may store type with surrounding quotes (e.g. "\"ORCA_PLUGIN_METADATA\"") due to escaping; trim them
            std::string trimmed = type_str;
            auto trim = [](std::string s){
                size_t a=0; while(a<s.size() && isspace((unsigned char)s[a])) ++a;
                size_t b=s.size(); while(b>a && isspace((unsigned char)s[b-1])) --b;
                s=s.substr(a,b-a);
                if(s.size()>=2 && s.front()=='"' && s.back()=='"') s=s.substr(1,s.size()-2);
                if(s.size()>=2 && s.front()=='\'' && s.back()=='\'') s=s.substr(1,s.size()-2);
                return s;
            };
            trimmed = trim(type_str);
            if (trimmed == PLUGIN_PE_RESOURCE_TYPE || type_str == PLUGIN_PE_RESOURCE_TYPE) {
                if ((e.OffsetToData & 0x80000000u) == 0) { error = "PE: resource type is not directory"; return false; }
                type_dir_off = e.OffsetToData & 0x7FFFFFFFu;
                found_type = true;
                break;
            }
        } else {
            continue;
        }
        (void)id;
    }
    if (!found_type) { error = std::string("PE: resource type '") + PLUGIN_PE_RESOURCE_TYPE + "' not found"; return false; }

    ResDirHeader name_hdr{};
    std::vector<ResDirEntry> name_entries;
    if (!parse_dir(type_dir_off, name_entries, name_hdr)) { error = "PE: cannot parse type directory"; return false; }

    uint32_t lang_dir_off = 0;
    bool found_name = false;
    for (auto &e : name_entries) {
        bool is_named = (e.NameOrId & 0x80000000u) != 0;
        uint32_t id = e.NameOrId & 0x7FFFFFFFu;
        if (is_named) {
            // Resource names must be numeric id 1.
            continue;
        } else {
            if (id == (uint32_t)PLUGIN_PE_RESOURCE_ID) {
                if ((e.OffsetToData & 0x80000000u) == 0) { error = "PE: resource name is not directory"; return false; }
                lang_dir_off = e.OffsetToData & 0x7FFFFFFFu;
                found_name = true;
                break;
            }
        }
    }
    if (!found_name) { error = "PE: resource id 1 not found under " + std::string(PLUGIN_PE_RESOURCE_TYPE); return false; }

    ResDirHeader lang_hdr{};
    std::vector<ResDirEntry> lang_entries;
    if (!parse_dir(lang_dir_off, lang_entries, lang_hdr)) { error = "PE: cannot parse language directory"; return false; }

    ResDataEntry data_entry{};
    bool found_data = false;
    for (auto &e : lang_entries) {
        // Language-level entries point to data.
        if (e.OffsetToData & 0x80000000u) {
            continue;
        }
        uint32_t data_off = e.OffsetToData;
        size_t abs = (size_t)res_offset + (size_t)data_off;
        if (abs + sizeof(ResDataEntry) > image.size()) continue;
        if (abs + sizeof(ResDataEntry) > (size_t)res_offset + res_size) continue;
        memcpy(&data_entry, image.data()+abs, sizeof(data_entry));
        found_data = true;
        break; // take first language
    }
    if (!found_data) { error = "PE: resource data entry not found for id 1"; return false; }

    uint32_t data_rva = data_entry.OffsetToData;
    uint32_t data_size = data_entry.Size;
    if (data_size == 0 || data_size > 1024 * 1024) { error = "PE: invalid resource data size"; return false; }
    uint32_t data_file_off = 0;
    if (!rva_to_file_offset(data_rva, sections, data_file_off)) {
        error = "PE: resource data RVA not mapped";
        return false;
    }
    if ((size_t)data_file_off + data_size > image.size()) {
        error = "PE: resource data extends beyond file";
        return false;
    }
    res_bytes.assign(image.begin() + data_file_off, image.begin() + data_file_off + data_size);
    while (!res_bytes.empty() && res_bytes.back() == 0) res_bytes.pop_back();
    if (res_bytes.empty()) { error = "PE: resource data empty"; return false; }
    return true;
}


struct Elf64_Ehdr {
    unsigned char e_ident[16];
    uint16_t e_type;
    uint16_t e_machine;
    uint32_t e_version;
    uint64_t e_entry;
    uint64_t e_phoff;
    uint64_t e_shoff;
    uint32_t e_flags;
    uint16_t e_ehsize;
    uint16_t e_phentsize;
    uint16_t e_phnum;
    uint16_t e_shentsize;
    uint16_t e_shnum;
    uint16_t e_shstrndx;
};
struct Elf64_Shdr {
    uint32_t sh_name;
    uint32_t sh_type;
    uint64_t sh_flags;
    uint64_t sh_addr;
    uint64_t sh_offset;
    uint64_t sh_size;
    uint32_t sh_link;
    uint32_t sh_info;
    uint64_t sh_addralign;
    uint64_t sh_entsize;
};
struct Elf_Nhdr {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
};

} // anon

bool read_pe_metadata(const boost::filesystem::path &dll_path, std::string &json_text, std::string &error)
{
    std::vector<unsigned char> image;
    if (!read_file_bytes(dll_path, image, error)) return false;
    std::vector<unsigned char> res;
    if (!extract_pe_resource_bytes(image, res, error)) return false;
    json_text.assign((char*)res.data(), res.size());
    if (json_text.empty() || json_text.size() > 64*1024) { error = "PE resource metadata too large or empty"; return false; }
    try {
        auto j = nlohmann::json::parse(json_text, nullptr, true, true);
        if (j.is_discarded() || !j.is_object()) { error = "PE resource is not a JSON object"; return false; }
    } catch (const std::exception &e) {
        error = std::string("PE resource JSON parse error: ") + e.what();
        return false;
    }
    return true;
}

bool read_pe_metadata(const boost::filesystem::path &dll_path, PluginMetadata &out, std::string &error)
{
    std::string txt;
    if (!read_pe_metadata(dll_path, txt, error)) return false;
    return validate_plugin_metadata_json(txt, out, error);
}

bool read_elf_metadata(const boost::filesystem::path &so_path, std::string &json_text, std::string &error)
{
    std::vector<unsigned char> image;
    if (!read_file_bytes(so_path, image, error)) return false;
    if (image.size() < sizeof(Elf64_Ehdr)) { error = "ELF too small"; return false; }
    if (image[0] != 0x7F || image[1] != 'E' || image[2] != 'L' || image[3] != 'F') {
        error = "ELF missing magic";
        return false;
    }
    unsigned char ei_class = image[4];
    unsigned char ei_data  = image[5];
    if (ei_class != 2) { error = "ELF not 64-bit"; return false; } // only support ELF64
    bool is_le = (ei_data == 1);
    if (!is_le) { error = "ELF not little endian"; return false; }

    Elf64_Ehdr ehdr{};
    memcpy(&ehdr, image.data(), sizeof(ehdr));
    if (ehdr.e_shoff == 0 || ehdr.e_shnum == 0 || ehdr.e_shnum > 128) { error = "ELF invalid section header table"; return false; }
    if (ehdr.e_shentsize != sizeof(Elf64_Shdr)) { error = "ELF section header entry size mismatch"; return false; }
    if (ehdr.e_shstrndx >= ehdr.e_shnum) { error = "ELF e_shstrndx out of range"; return false; }
    if (ehdr.e_shoff + (size_t)ehdr.e_shnum * sizeof(Elf64_Shdr) > image.size()) { error = "ELF section headers out of range"; return false; }

    std::vector<Elf64_Shdr> shdrs(ehdr.e_shnum);
    for (int i=0;i<ehdr.e_shnum;++i) memcpy(&shdrs[i], image.data()+ehdr.e_shoff + i*sizeof(Elf64_Shdr), sizeof(Elf64_Shdr));

    const Elf64_Shdr &shstr = shdrs[ehdr.e_shstrndx];
    if (shstr.sh_offset + shstr.sh_size > image.size()) { error = "ELF shstrtab out of range"; return false; }
    const unsigned char *shstr_data = image.data() + shstr.sh_offset;

    int target_idx = -1;
    for (int i=0;i<ehdr.e_shnum;++i) {
        if (shdrs[i].sh_name >= shstr.sh_size) continue;
        const char *name = (const char*)shstr_data + shdrs[i].sh_name;
        size_t max_len = shstr.sh_size - shdrs[i].sh_name;
        size_t len = strnlen(name, max_len);
        if (len < max_len && strcmp(name, PLUGIN_ELF_NOTE_SECTION) == 0) {
            target_idx = i;
            break;
        }
    }
    if (target_idx < 0) { error = std::string("ELF missing section ") + PLUGIN_ELF_NOTE_SECTION; return false; }
    const auto &note_sec = shdrs[target_idx];
    if (note_sec.sh_size == 0 || note_sec.sh_size > 64*1024) { error = "ELF note section size invalid"; return false; }
    if (note_sec.sh_offset + note_sec.sh_size > image.size()) { error = "ELF note section out of range"; return false; }
    const unsigned char *note_data = image.data() + note_sec.sh_offset;
    size_t note_size = (size_t)note_sec.sh_size;

    // Accept raw JSON section data before falling back to note parsing.
    {
        size_t start = 0;
        while (start < note_size && (note_data[start]=='\0' || isspace(note_data[start]))) ++start;
        if (start < note_size && note_data[start] == '{') {
            size_t end = note_size;
            while (end > start && (note_data[end-1]=='\0' || isspace(note_data[end-1]))) --end;
            std::string candidate((char*)note_data+start, end-start);
            try {
                auto j = nlohmann::json::parse(candidate, nullptr, true, true);
                if (!j.is_discarded() && j.is_object()) {
                    json_text = candidate;
                    return true;
                }
            } catch (...) {
            }
        }
    }

    // ELF notes are Nhdr + name + desc padded to 4 bytes.
    size_t off = 0;
    std::string found;
    bool any = false;
    while (off + sizeof(Elf_Nhdr) <= note_size) {
        Elf_Nhdr nhdr{};
        memcpy(&nhdr, note_data+off, sizeof(nhdr));
        // ELF note fields are little endian; host is little endian, so direct.
        if (nhdr.n_namesz > 1024 || nhdr.n_descsz > 64*1024) { error = "ELF note name/desc size invalid"; return false; }
        off += sizeof(Elf_Nhdr);
        size_t namesz_padded = (nhdr.n_namesz + 3) & ~3u;
        size_t descsz_padded = (nhdr.n_descsz + 3) & ~3u;
        if (off + namesz_padded + descsz_padded > note_size) { error = "ELF note truncated"; return false; }
        std::string name;
        if (nhdr.n_namesz > 0) {
                const char *nptr = (const char*)note_data + off;
            size_t nlen = strnlen(nptr, nhdr.n_namesz);
            name.assign(nptr, nlen);
        }
        const unsigned char *desc_ptr = note_data + off + namesz_padded;
        size_t desc_len = (size_t)nhdr.n_descsz;
        if (name == PLUGIN_ELF_NOTE_NAME) {
            while (desc_len > 0 && desc_ptr[desc_len-1] == '\0') --desc_len;
            found.assign((char*)desc_ptr, desc_len);
            try {
                auto j = nlohmann::json::parse(found, nullptr, true, true);
                if (!j.is_discarded() && j.is_object()) {
                    json_text = found;
                    any = true;
                    break;
                } else {
                    error = "ELF note desc is not a JSON object";
                    return false;
                }
            } catch (const std::exception &e) {
                error = std::string("ELF note JSON parse error: ") + e.what();
                return false;
            }
        }
        off += namesz_padded + descsz_padded;
    }
    if (any) return true;
    if (!found.empty()) {
        error = "ELF note found but JSON invalid";
        return false;
    }
    error = "ELF note '" + std::string(PLUGIN_ELF_NOTE_NAME) + "' not found in " + PLUGIN_ELF_NOTE_SECTION;
    return false;
}

bool read_elf_metadata(const boost::filesystem::path &so_path, PluginMetadata &out, std::string &error)
{
    std::string txt;
    if (!read_elf_metadata(so_path, txt, error)) return false;
    return validate_plugin_metadata_json(txt, out, error);
}

bool read_jar_metadata(const boost::filesystem::path &jar_path, std::string &json_text, std::string &error)
{
    // Only the known entry is extracted, without executing code.

    mz_zip_archive zip{};
    mz_zip_zero_struct(&zip);
    std::string fname = jar_path.string();
    if (!mz_zip_reader_init_file(&zip, fname.c_str(), 0)) {
        error = "JAR: cannot open zip: " + std::string(mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return false;
    }
    struct Guard {
        mz_zip_archive *z;
        ~Guard(){ mz_zip_reader_end(z); }
    } guard{&zip};

    mz_uint num = mz_zip_reader_get_num_files(&zip);
    if (num == 0) { error = "JAR: empty zip"; return false; }
    if (num > 8192) { error = "JAR: too many entries"; return false; }

    mz_uint idx = (mz_uint)-1;
    for (mz_uint i=0;i<num;++i) {
        mz_zip_archive_file_stat stat{};
        if (!mz_zip_reader_file_stat(&zip, i, &stat)) continue;
        std::string name = stat.m_filename;
        if (name == PLUGIN_JAR_ENTRY) { idx = i; break; }
    }
    if (idx == (mz_uint)-1) {
        error = std::string("JAR: missing entry ") + PLUGIN_JAR_ENTRY;
        return false;
    }
    mz_zip_archive_file_stat stat{};
    if (!mz_zip_reader_file_stat(&zip, idx, &stat)) {
        error = "JAR: cannot stat entry";
        return false;
    }
    if (stat.m_uncomp_size > 64*1024) { error = "JAR: plugin.json too large"; return false; }
    if (stat.m_uncomp_size == 0) { error = "JAR: plugin.json empty"; return false; }

    std::string buf;
    buf.resize((size_t)stat.m_uncomp_size);
    if (!mz_zip_reader_extract_to_mem(&zip, idx, (void*)buf.data(), buf.size(), 0)) {
        error = "JAR: cannot extract entry: " + std::string(mz_zip_get_error_string(mz_zip_get_last_error(&zip)));
        return false;
    }
    // Reject entries with traversal paths (zip slip).
    for (mz_uint i=0;i<num;++i) {
        mz_zip_archive_file_stat s{};
        if (!mz_zip_reader_file_stat(&zip, i, &s)) continue;
        std::string n = s.m_filename;
        if (n.find("..") != std::string::npos) {
            error = "JAR: entry with '..' path traversal: " + n;
            return false;
        }
        if (!n.empty() && (n[0]=='/' || n[0]=='\\')) {
            error = "JAR: entry with absolute path: " + n;
            return false;
        }
    }
    // Reject UTF-8 BOM.
    if (buf.size()>=3 && (unsigned char)buf[0]==0xEF && (unsigned char)buf[1]==0xBB && (unsigned char)buf[2]==0xBF) {
        error = "JAR: plugin.json must not contain UTF-8 BOM";
        return false;
    }
    try {
        auto j = nlohmann::json::parse(buf, nullptr, true, true);
        if (j.is_discarded() || !j.is_object()) { error = "JAR: plugin.json is not a JSON object"; return false; }
    } catch (const std::exception &e) {
        error = std::string("JAR: plugin.json parse error: ") + e.what();
        return false;
    }
    json_text = buf;
    return true;
}

bool read_jar_metadata(const boost::filesystem::path &jar_path, PluginMetadata &out, std::string &error)
{
    std::string txt;
    if (!read_jar_metadata(jar_path, txt, error)) return false;
    return validate_plugin_metadata_json(txt, out, error);
}

bool read_plugin_metadata_file(const boost::filesystem::path &path, std::string &json_text, std::string &error)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    if (ext == ".dll") return read_pe_metadata(path, json_text, error);
    if (ext == ".so")  return read_elf_metadata(path, json_text, error);
    if (ext == ".jar") return read_jar_metadata(path, json_text, error);
    error = "unknown plugin artifact extension '" + ext + "' (expected .dll/.so/.jar)";
    return false;
}

bool read_plugin_metadata_file(const boost::filesystem::path &path, PluginMetadata &out, std::string &error)
{
    std::string txt;
    if (!read_plugin_metadata_file(path, txt, error)) return false;
    return validate_plugin_metadata_json(txt, out, error);
}

InspectResult inspect_plugin_file(const boost::filesystem::path &path)
{
    std::string dummy;
    return inspect_plugin_file(path, dummy);
}

InspectResult inspect_plugin_file(const boost::filesystem::path &path, std::string &error_out)
{
    InspectResult r;
    boost::system::error_code ec;
    if (!boost::filesystem::exists(path, ec) || !boost::filesystem::is_regular_file(path, ec)) {
        r.error = "file not found or not regular file: " + path.string();
        error_out = r.error;
        return r;
    }
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    if (ext != ".dll" && ext != ".so" && ext != ".jar") {
        r.error = "unsupported plugin artifact extension '" + ext + "'";
        error_out = r.error;
        return r;
    }
    // Compute the hash even if metadata validation fails.
    std::string hash_err;
    std::string h = sha256_file_hex(path, hash_err);
    if (h.empty()) {
        r.error = hash_err;
        error_out = r.error;
        return r;
    }
    r.artifact_hash = h;

    std::string meta_err;
    PluginMetadata meta;
    bool ok = read_plugin_metadata_file(path, meta, meta_err);
    if (!ok) {
        r.error = meta_err;
        error_out = r.error;
        return r;
    }
    std::string json_txt;
    if (ext == ".dll") read_pe_metadata(path, json_txt, meta_err);
    else if (ext == ".so") read_elf_metadata(path, json_txt, meta_err);
    else read_jar_metadata(path, json_txt, meta_err);
    r.metadata = std::move(meta);
    r.json_text = std::move(json_txt);
    r.ok = true;
    error_out.clear();
    return r;
}

bool is_safe_plugin_id_for_fs(const std::string &id)
{
    return is_valid_plugin_id(id);
}

bool is_allowed_artifact_extension(const boost::filesystem::path &path, std::string &normalized_ext)
{
    std::string ext = path.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), [](unsigned char c){ return std::tolower(c); });
    normalized_ext = ext;
#ifdef _WIN32
    if (ext == ".dll" || ext == ".jar") return true;
#else
    if (ext == ".so" || ext == ".jar") return true;
#endif
    // Accept all artifact extensions during inspection regardless of host.
    if (ext == ".dll" || ext == ".so" || ext == ".jar") return true;
    return false;
}

} // namespace Slic3r::Plugin::Package
