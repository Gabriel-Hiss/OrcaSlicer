#include "orca/hook_sdkgen/pe_image.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <stdexcept>
#include <algorithm>

namespace orca::hook_sdkgen {

std::array<uint8_t,32> sha256_file(const std::string& path) {
    std::ifstream f(path, std::ios::binary);
    if (!f) throw GenerationError("cannot open file for hashing: " + path);
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    if (sz > 0) {
        f.read(reinterpret_cast<char*>(data.data()), sz);
        if ((size_t)f.gcount() != sz) throw GenerationError("short read for hash: " + path);
    }
    return sha256(data.data(), data.size());
}
std::array<uint8_t,32> sha256_bytes(const uint8_t* data, size_t len){ return sha256(data, len); }

#pragma pack(push,1)
struct DosHeader { uint16_t e_magic; uint16_t e_cblp; uint16_t e_cp; uint16_t e_crlc; uint16_t e_cparhdr; uint16_t e_minalloc; uint16_t e_maxalloc; uint16_t e_ss; uint16_t e_sp; uint16_t e_csum; uint16_t e_ip; uint16_t e_cs; uint16_t e_lfarlc; uint16_t e_ovno; uint16_t e_res[4]; uint16_t e_oemid; uint16_t e_oeminfo; uint16_t e_res2[10]; int32_t e_lfanew; };
struct DataDirectory { uint32_t VirtualAddress; uint32_t Size; };
struct FileHeader { uint16_t Machine; uint16_t NumberOfSections; uint32_t TimeDateStamp; uint32_t PointerToSymbolTable; uint32_t NumberOfSymbols; uint16_t SizeOfOptionalHeader; uint16_t Characteristics; };
struct OptionalHeader64 {
    uint16_t Magic; uint8_t MajorLinkerVersion; uint8_t MinorLinkerVersion; uint32_t SizeOfCode; uint32_t SizeOfInitializedData; uint32_t SizeOfUninitializedData; uint32_t AddressOfEntryPoint; uint32_t BaseOfCode; uint64_t ImageBase; uint32_t SectionAlignment; uint32_t FileAlignment; uint16_t MajorOSVersion; uint16_t MinorOSVersion; uint16_t MajorImageVersion; uint16_t MinorImageVersion; uint16_t MajorSubsystemVersion; uint16_t MinorSubsystemVersion; uint32_t Win32VersionValue; uint32_t SizeOfImage; uint32_t SizeOfHeaders; uint32_t CheckSum; uint16_t Subsystem; uint16_t DllCharacteristics; uint64_t SizeOfStackReserve; uint64_t SizeOfStackCommit; uint64_t SizeOfHeapReserve; uint64_t SizeOfHeapCommit; uint32_t LoaderFlags; uint32_t NumberOfRvaAndSizes; DataDirectory DataDirectoryEntries[16];
};
struct OptionalHeader32 {
    uint16_t Magic; uint8_t MajorLinkerVersion; uint8_t MinorLinkerVersion; uint32_t SizeOfCode; uint32_t SizeOfInitializedData; uint32_t SizeOfUninitializedData; uint32_t AddressOfEntryPoint; uint32_t BaseOfCode; uint32_t BaseOfData; uint32_t ImageBase; uint32_t SectionAlignment; uint32_t FileAlignment; uint16_t MajorOSVersion; uint16_t MinorOSVersion; uint16_t MajorImageVersion; uint16_t MinorImageVersion; uint16_t MajorSubsystemVersion; uint16_t MinorSubsystemVersion; uint32_t Win32VersionValue; uint32_t SizeOfImage; uint32_t SizeOfHeaders; uint32_t CheckSum; uint16_t Subsystem; uint16_t DllCharacteristics; uint32_t SizeOfStackReserve; uint32_t SizeOfStackCommit; uint32_t SizeOfHeapReserve; uint32_t SizeOfHeapCommit; uint32_t LoaderFlags; uint32_t NumberOfRvaAndSizes; DataDirectory DataDirectoryEntries[16];
};
struct SectionHeader { char Name[8]; uint32_t VirtualSize; uint32_t VirtualAddress; uint32_t SizeOfRawData; uint32_t PointerToRawData; uint32_t PointerToRelocations; uint32_t PointerToLinenumbers; uint16_t NumberOfRelocations; uint16_t NumberOfLinenumbers; uint32_t Characteristics; };
struct DebugDirectoryEntry { uint32_t Characteristics; uint32_t TimeDateStamp; uint16_t MajorVersion; uint16_t MinorVersion; uint32_t Type; uint32_t SizeOfData; uint32_t AddressOfRawData; uint32_t PointerToRawData; };
struct CvInfoPdb70 { uint32_t CvSignature; uint8_t Guid[16]; uint32_t Age; char PdbFileName[1]; };
#pragma pack(pop)

static bool is_pe64(const uint8_t* data, size_t sz, size_t opt_offset) {
    if (opt_offset + 2 > sz) return false;
    uint16_t magic = data[opt_offset] | (data[opt_offset+1]<<8);
    return magic == 0x20b; // PE32+
}

PeInfo read_pe_info(const std::string& pe_path) {
    std::ifstream f(pe_path, std::ios::binary);
    if (!f) throw GenerationError("cannot open PE: " + pe_path);
    f.seekg(0, std::ios::end);
    size_t file_size = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(file_size);
    f.read(reinterpret_cast<char*>(data.data()), file_size);
    if ((size_t)f.gcount() != file_size) throw GenerationError("short read PE: " + pe_path);
    if (file_size < sizeof(DosHeader)) throw GenerationError("PE too small");
    DosHeader dos{}; std::memcpy(&dos, data.data(), sizeof(dos));
    if (dos.e_magic != 0x5A4D) throw GenerationError("not a PE file (MZ missing): " + pe_path);
    if ((size_t)dos.e_lfanew + 6 > file_size) throw GenerationError("PE lfanew out of range");
    uint32_t pe_sig = data[dos.e_lfanew] | (data[dos.e_lfanew+1]<<8) | (data[dos.e_lfanew+2]<<16) | (data[dos.e_lfanew+3]<<24);
    if (pe_sig != 0x00004550) throw GenerationError("not a PE file (PE sig missing)");
    size_t fh_off = dos.e_lfanew + 4;
    if (fh_off + sizeof(FileHeader) > file_size) throw GenerationError("file header out of range");
    FileHeader fh{}; std::memcpy(&fh, data.data()+fh_off, sizeof(fh));
    size_t opt_off = fh_off + sizeof(FileHeader);
    bool pe64 = is_pe64(data.data(), file_size, opt_off);
    uint32_t entry_rva = 0;
    uint32_t debug_rva = 0, debug_size = 0;
    uint32_t import_rva = 0, import_size = 0;
    if (pe64) {
        if (opt_off + sizeof(OptionalHeader64) > file_size) throw GenerationError("optional header out of range");
        OptionalHeader64 oh{}; std::memcpy(&oh, data.data()+opt_off, sizeof(oh));
        entry_rva = oh.AddressOfEntryPoint;
        if (oh.NumberOfRvaAndSizes > 1) { import_rva = oh.DataDirectoryEntries[1].VirtualAddress; import_size = oh.DataDirectoryEntries[1].Size; }
        if (oh.NumberOfRvaAndSizes > 6) { debug_rva = oh.DataDirectoryEntries[6].VirtualAddress; debug_size = oh.DataDirectoryEntries[6].Size; }
    } else {
        if (opt_off + sizeof(OptionalHeader32) > file_size) throw GenerationError("optional header out of range");
        OptionalHeader32 oh{}; std::memcpy(&oh, data.data()+opt_off, sizeof(oh));
        entry_rva = oh.AddressOfEntryPoint;
        if (oh.NumberOfRvaAndSizes > 1) { import_rva = oh.DataDirectoryEntries[1].VirtualAddress; import_size = oh.DataDirectoryEntries[1].Size; }
        if (oh.NumberOfRvaAndSizes > 6) { debug_rva = oh.DataDirectoryEntries[6].VirtualAddress; debug_size = oh.DataDirectoryEntries[6].Size; }
    }
    size_t sec_off = opt_off + fh.SizeOfOptionalHeader;
    std::vector<PeInfo::Section> secs;
    secs.reserve(fh.NumberOfSections);
    for (int i=0;i<fh.NumberOfSections;++i){
        size_t off = sec_off + i*sizeof(SectionHeader);
        if (off + sizeof(SectionHeader) > file_size) throw GenerationError("section header out of range");
        SectionHeader sh{}; std::memcpy(&sh, data.data()+off, sizeof(sh));
        PeInfo::Section s;
        char name[9]={}; std::memcpy(name, sh.Name, 8);
        s.name = name;
        s.virtual_address = sh.VirtualAddress;
        s.virtual_size = sh.VirtualSize;
        s.raw_offset = sh.PointerToRawData;
        s.raw_size = sh.SizeOfRawData;
        s.characteristics = sh.Characteristics;
        secs.push_back(std::move(s));
    }
    auto rva_to_file_offset = [&](uint32_t rva)->size_t{
        for (auto &s: secs) if (rva >= s.virtual_address && rva < s.virtual_address + std::max(s.virtual_size, s.raw_size)) return (size_t)(rva - s.virtual_address + s.raw_offset);
        return SIZE_MAX;
    };

    PeInfo info;
    info.sections = secs;
    info.entry_rva = entry_rva;
    info.import_rva = import_rva;
    info.import_size = import_size;
    info.debug_rva = debug_rva;
    info.debug_size = debug_size;
    info.sha256 = sha256(data.data(), data.size());
    if (debug_rva != 0 && debug_size != 0) {
        size_t dbg_off = rva_to_file_offset(debug_rva);
        if (dbg_off != SIZE_MAX && dbg_off + debug_size <= file_size) {
            size_t count = debug_size / sizeof(DebugDirectoryEntry);
            for (size_t i=0;i<count;++i){
                size_t off = dbg_off + i*sizeof(DebugDirectoryEntry);
                DebugDirectoryEntry de{}; std::memcpy(&de, data.data()+off, sizeof(de));
                if (de.Type != 2) continue; // IMAGE_DEBUG_TYPE_CODEVIEW
                size_t raw = de.PointerToRawData;
                uint32_t szd = de.SizeOfData;
                if (raw + szd > file_size) continue;
                if (szd < sizeof(CvInfoPdb70)) continue;
                uint32_t sig = data[raw] | (data[raw+1]<<8) | (data[raw+2]<<16) | (data[raw+3]<<24);
                if (sig != 0x53445352) continue; // 'RSDS'
                CvInfoPdb70 cv{}; std::memcpy(cv.Guid, data.data()+raw+4, 16); std::memcpy(&cv.Age, data.data()+raw+20, 4);
                // Guid stored as little-endian for first 3 fields already raw; we keep raw bytes
                info.codeview.found = true;
                std::memcpy(info.codeview.guid.data(), cv.Guid, 16);
                info.codeview.age = cv.Age;
                // Pdb file name is null-terminated after 24 bytes
                size_t name_off = raw + 24;
                size_t name_len = 0;
                while (name_off + name_len < file_size && data[name_off+name_len]!=0) ++name_len;
                info.codeview.pdb_file_name.assign(reinterpret_cast<char*>(data.data()+name_off), name_len);
                break;
            }
        }
    }
    return info;
}

std::string rva_to_section_name(const PeInfo& info, uint32_t rva){
    for (auto &s: info.sections) if (rva >= s.virtual_address && rva < s.virtual_address + std::max(s.virtual_size, s.raw_size)) return s.name;
    return "";
}

} // namespace
