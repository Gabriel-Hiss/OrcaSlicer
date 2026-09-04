#include "orca/hook_sdkgen/elf_image.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include <fstream>
#include <vector>
#include <cstring>
#include <algorithm>
#include <cctype>
#include <stdexcept>

namespace orca::hook_sdkgen {

#pragma pack(push,1)
struct Elf64_Ehdr {
    uint8_t  e_ident[16];
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
struct Elf64_Phdr {
    uint32_t p_type;
    uint32_t p_flags;
    uint64_t p_offset;
    uint64_t p_vaddr;
    uint64_t p_paddr;
    uint64_t p_filesz;
    uint64_t p_memsz;
    uint64_t p_align;
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
struct Elf64_Sym {
    uint32_t st_name;
    uint8_t  st_info;
    uint8_t  st_other;
    uint16_t st_shndx;
    uint64_t st_value;
    uint64_t st_size;
};
struct Elf64_Rela {
    uint64_t r_offset;
    uint64_t r_info;
    int64_t  r_addend;
};
struct Elf64_Nhdr {
    uint32_t n_namesz;
    uint32_t n_descsz;
    uint32_t n_type;
};
#pragma pack(pop)

static constexpr uint8_t EI_CLASS = 4, EI_DATA = 5;
static constexpr uint8_t ELFCLASS64 = 2, ELFDATA2LSB = 1;
static constexpr uint16_t EM_X86_64 = 62;
static constexpr uint32_t PT_LOAD = 1, PT_NOTE = 4, PT_GNU_STACK = 0x6474e551;
static constexpr uint32_t SHT_NULL = 0, SHT_PROGBITS = 1, SHT_SYMTAB = 2, SHT_STRTAB = 3, SHT_RELA = 4, SHT_NOTE = 7, SHT_DYNSYM = 11;
static constexpr uint32_t NT_GNU_BUILD_ID = 3;
static constexpr uint8_t STB_LOCAL = 0, STB_GLOBAL = 1, STB_WEAK = 2;
static constexpr uint8_t STT_NOTYPE = 0, STT_OBJECT = 1, STT_FUNC = 2;
static constexpr uint32_t R_X86_64_NONE = 0, R_X86_64_64 = 1, R_X86_64_GLOB_DAT = 6, R_X86_64_JUMP_SLOT = 7;

static std::string to_lower_str(std::string s){ for(char& c: s) c= (char)tolower((unsigned char)c); return s; }

ElfInfo read_elf_info(const std::string& elf_path) {
    std::ifstream f(elf_path, std::ios::binary);
    if (!f) throw GenerationError("cannot open ELF: " + elf_path);
    f.seekg(0, std::ios::end);
    size_t sz = (size_t)f.tellg();
    f.seekg(0, std::ios::beg);
    std::vector<uint8_t> data(sz);
    if (sz>0) { f.read(reinterpret_cast<char*>(data.data()), sz); if (!f) throw GenerationError("read failed: " + elf_path); }

    if (sz < sizeof(Elf64_Ehdr)) throw GenerationError("ELF too small: " + elf_path);
    auto* eh = reinterpret_cast<Elf64_Ehdr*>(data.data());
    if (eh->e_ident[0]!=0x7f || eh->e_ident[1]!='E' || eh->e_ident[2]!='L' || eh->e_ident[3]!='F')
        throw GenerationError("not an ELF file: " + elf_path);
    if (eh->e_ident[EI_CLASS] != ELFCLASS64) throw GenerationError("only ELF64 supported: " + elf_path);
    if (eh->e_ident[EI_DATA] != ELFDATA2LSB) throw GenerationError("only little endian ELF supported: " + elf_path);
    if (eh->e_machine != EM_X86_64) throw GenerationError("only x86_64 ELF supported: " + elf_path);

    ElfInfo info;
    info.is_64 = true;
    info.is_little = true;
    info.machine = eh->e_machine;
    info.entry = eh->e_entry;
    info.elf_class = eh->e_ident[EI_CLASS];
    info.sha256 = sha256(data.data(), data.size());

    if (eh->e_phoff + (size_t)eh->e_phnum * sizeof(Elf64_Phdr) > sz) throw GenerationError("phdr out of range");
    uint64_t low_vaddr = UINT64_MAX;
    for (int i=0;i<eh->e_phnum;++i){
        auto* ph = reinterpret_cast<Elf64_Phdr*>(data.data() + eh->e_phoff + i*sizeof(Elf64_Phdr));
        ElfSegment seg; seg.type=ph->p_type; seg.flags=ph->p_flags; seg.offset=ph->p_offset; seg.vaddr=ph->p_vaddr; seg.filesz=ph->p_filesz; seg.memsz=ph->p_memsz; seg.align=ph->p_align;
        info.segments.push_back(seg);
        if (ph->p_type == PT_LOAD && ph->p_vaddr < low_vaddr) low_vaddr = ph->p_vaddr;
        if (ph->p_type == PT_NOTE) {
            // Scan notes for GNU build-id (may also be in SHT_NOTE, but check PT_NOTE as well)
            size_t off = (size_t)ph->p_offset;
            size_t end = off + (size_t)ph->p_filesz;
            if (end <= sz) {
                size_t cur = off;
                while (cur + sizeof(Elf64_Nhdr) <= end) {
                    auto* nh = reinterpret_cast<Elf64_Nhdr*>(data.data()+cur);
                    uint32_t namesz = nh->n_namesz; uint32_t descsz = nh->n_descsz; uint32_t type = nh->n_type;
                    size_t name_off = cur + sizeof(Elf64_Nhdr);
                    size_t desc_off = name_off + ((namesz+3)&~3u);
                    size_t next = desc_off + ((descsz+3)&~3u);
                    if (next > end) break;
                    std::string name;
                    if (namesz>0 && name_off+namesz <= sz) name.assign(reinterpret_cast<char*>(data.data()+name_off), namesz);
                    // name is "GNU\0" for build-id
                    if (type == NT_GNU_BUILD_ID && name.rfind("GNU",0)==0) {
                        info.gnu_build_id.found = true;
                        info.gnu_build_id.bytes.assign(data.data()+desc_off, data.data()+desc_off+descsz);
                        info.gnu_build_id.hex = hex_bytes(data.data()+desc_off, descsz);
                    }
                    cur = next;
                }
            }
        }
    }
    if (low_vaddr==UINT64_MAX) low_vaddr = 0;
    info.image_base_vaddr = low_vaddr;

    if (eh->e_shoff==0 || eh->e_shnum==0) throw GenerationError("no section headers; ELF may be stripped without symtab: " + elf_path);
    if (eh->e_shoff + (size_t)eh->e_shnum * sizeof(Elf64_Shdr) > sz) throw GenerationError("shdr out of range");
    std::vector<Elf64_Shdr> shdrs(eh->e_shnum);
    for (int i=0;i<eh->e_shnum;++i) shdrs[i]= *reinterpret_cast<Elf64_Shdr*>(data.data()+ eh->e_shoff + i*sizeof(Elf64_Shdr));

    std::string shstr;
    if (eh->e_shstrndx < eh->e_shnum) {
        auto& sh = shdrs[eh->e_shstrndx];
        if (sh.sh_offset + sh.sh_size <= sz) shstr.assign(reinterpret_cast<char*>(data.data()+sh.sh_offset), sh.sh_size);
    }
    auto sh_name = [&](uint32_t off)->std::string{
        if (off >= shstr.size()) return "";
        const char* p = shstr.c_str()+off;
        return std::string(p);
    };
    for (int i=0;i<eh->e_shnum;++i){
        auto& sh = shdrs[i];
        ElfSection sec; sec.name = sh_name(sh.sh_name); sec.type=sh.sh_type; sec.flags=sh.sh_flags; sec.virtual_address=sh.sh_addr; sec.file_offset=sh.sh_offset; sec.size=sh.sh_size; sec.link=sh.sh_link; sec.info=sh.sh_info; sec.addralign=sh.sh_addralign; sec.entsize=sh.sh_entsize;
        info.sections.push_back(std::move(sec));
    }
    auto find_sec = [&](const std::string& n)->const ElfSection*{
        for(auto& s: info.sections) if(s.name==n) return &s;
        return nullptr;
    };
    // GNU build-id from SHT_NOTE .note.gnu.build-id if not already found
    if (!info.gnu_build_id.found) {
        if (auto* s = find_sec(".note.gnu.build-id")) {
            size_t off = (size_t)s->file_offset; size_t end = off + (size_t)s->size;
            size_t cur = off;
            while (cur + sizeof(Elf64_Nhdr) <= end && cur + sizeof(Elf64_Nhdr) <= sz) {
                auto* nh = reinterpret_cast<Elf64_Nhdr*>(data.data()+cur);
                uint32_t namesz=nh->n_namesz, descsz=nh->n_descsz, type=nh->n_type;
                size_t name_off = cur + sizeof(Elf64_Nhdr);
                size_t desc_off = name_off + ((namesz+3)&~3u);
                size_t next = desc_off + ((descsz+3)&~3u);
                if (next > end) break;
                std::string name; if(namesz>0 && name_off+namesz<=sz) name.assign(reinterpret_cast<char*>(data.data()+name_off), namesz);
                if (type==NT_GNU_BUILD_ID && name.rfind("GNU",0)==0 && desc_off+descsz<=sz) {
                    info.gnu_build_id.found=true;
                    info.gnu_build_id.bytes.assign(data.data()+desc_off, data.data()+desc_off+descsz);
                    info.gnu_build_id.hex = hex_bytes(data.data()+desc_off, descsz);
                    break;
                }
                cur = next;
            }
        }
    }
    if (auto* ts = find_sec(".text")) {
        info.text_rva = va_to_rva(info, ts->virtual_address);
        info.text_size = (uint32_t)ts->size;
        if (ts->file_offset + ts->size <= sz) {
            info.text_bytes.assign(data.data()+ts->file_offset, data.data()+ts->file_offset+ts->size);
            info.text_sha256 = sha256(data.data()+ts->file_offset, ts->size);
        }
    }
    if (auto* dynstr = find_sec(".dynstr")) {
        // soname from .dynamic DT_SONAME not parsed yet; keep empty, use file name if needed
        (void)dynstr;
    }
    auto read_strtab = [&](const ElfSection* st)->std::string{
        if(!st || st->file_offset + st->size > sz) return "";
        return std::string(reinterpret_cast<char*>(data.data()+st->file_offset), st->size);
    };
    auto collect_symtab = [&](const ElfSection* sym_sec, const std::string& sec_name, std::vector<ElfSymbol>& out, bool is_dyn){
        if(!sym_sec) return;
        if(sym_sec->entsize != sizeof(Elf64_Sym)) throw GenerationError("sym entsize mismatch in " + sec_name);
        if(sym_sec->link >= info.sections.size()) throw GenerationError("sym link out of range");
        const ElfSection& str_sec = info.sections[sym_sec->link];
        std::string strtab = read_strtab(&str_sec);
        size_t count = sym_sec->size / sizeof(Elf64_Sym);
        size_t off = (size_t)sym_sec->file_offset;
        for(size_t i=0;i<count;++i){
            if(off + sizeof(Elf64_Sym) > sz) break;
            auto* sym = reinterpret_cast<Elf64_Sym*>(data.data()+off+i*sizeof(Elf64_Sym));
            uint8_t bind = sym->st_info >> 4;
            uint8_t type = sym->st_info & 0xf;
            if(sym->st_name >= strtab.size()) continue;
            const char* n = strtab.c_str()+sym->st_name;
            std::string name(n);
            if(name.empty()) continue;
            // Skip file and section symbols
            if(type==4 /*STT_SECTION*/ || type==STT_NOTYPE && sym->st_shndx==0) {
                // keep UND imports (shndx==0) for relocation matching even if NOTYPE
                if(sym->st_shndx!=0) continue;
            }
            ElfSymbol es; es.name=name; es.value=sym->st_value; es.size=sym->st_size; es.bind=bind; es.type=type; es.shndx=sym->st_shndx; es.is_import = (sym->st_shndx==0);
            if(sym->st_shndx < info.sections.size() && sym->st_shndx!=0) es.section_name = info.sections[sym->st_shndx].name;
            out.push_back(std::move(es));
        }
    };
    const ElfSection* symtab = find_sec(".symtab");
    const ElfSection* dynsym = find_sec(".dynsym");
    std::vector<ElfSymbol> all;
    collect_symtab(symtab, ".symtab", all, false);
    std::vector<ElfSymbol> dyn_only;
    collect_symtab(dynsym, ".dynsym", dyn_only, true);
    // Merge dyn symbols into all for lookup, keep dyn_only separate
    info.symbols = all;
    // Also append dyn symbols that are not already in symtab (by name+value)
    for(auto& d: dyn_only){
        bool dup=false;
        for(auto& a: info.symbols) if(a.name==d.name && a.value==d.value){ dup=true; break; }
        if(!dup) info.symbols.push_back(d);
    }
    info.dyn_symbols = dyn_only;

    auto collect_rela = [&](const std::string& sec_name){
        const ElfSection* rs = find_sec(sec_name);
        if(!rs) return;
        if(rs->type != SHT_RELA) return;
        // symtab for rela is link
        const ElfSection* sym_for_rela = nullptr;
        const std::string* symtab_str = nullptr;
        std::string tmp_str;
        if(rs->link < info.sections.size()){
            sym_for_rela = &info.sections[rs->link];
            tmp_str = read_strtab(&info.sections[sym_for_rela->link]);
            symtab_str = &tmp_str;
        }
        std::vector<ElfSymbol>* src_syms = nullptr;
        if(rs->name==".rela.plt" || rs->name==".rela.dyn"){
            // Both use .dynsym generally; but check link
            if(sym_for_rela && sym_for_rela->name==".dynsym") src_syms = &dyn_only;
            else src_syms = &all;
        }
        size_t count = rs->size / sizeof(Elf64_Rela);
        for(size_t i=0;i<count;++i){
            size_t off = (size_t)rs->file_offset + i*sizeof(Elf64_Rela);
            if(off+sizeof(Elf64_Rela)>sz) break;
            auto* re = reinterpret_cast<Elf64_Rela*>(data.data()+off);
            ElfRelocation r; r.offset=re->r_offset; r.addend=re->r_addend; r.section_name=sec_name;
            uint32_t sym_idx = (uint32_t)(re->r_info >> 32);
            uint32_t type = (uint32_t)(re->r_info & 0xffffffffu);
            r.sym_index=sym_idx; r.type=type;
            // Resolve symbol name
            if(src_syms && sym_idx < src_syms->size()){
                // Need random access by index: use direct symtab index, not vector order? For dynsym we have vector order == index.
                // Safer to read from raw symtab again by index.
                const ElfSection* s = sym_for_rela;
                if(s){
                    size_t sym_off = (size_t)s->file_offset + (size_t)sym_idx * sizeof(Elf64_Sym);
                    if(sym_off+sizeof(Elf64_Sym) <= sz){
                        auto* sym = reinterpret_cast<Elf64_Sym*>(data.data()+sym_off);
                        if (sym->st_name < tmp_str.size()){
                            const char* n = tmp_str.c_str()+sym->st_name;
                            r.sym_name = std::string(n);
                        }
                    }
                }
            } else if(symtab_str){
                const ElfSection* s = sym_for_rela;
                if(s && sym_idx * sizeof(Elf64_Sym) < s->size){
                    size_t sym_off = (size_t)s->file_offset + (size_t)sym_idx * sizeof(Elf64_Sym);
                    if(sym_off+sizeof(Elf64_Sym) <= sz){
                        auto* sym = reinterpret_cast<Elf64_Sym*>(data.data()+sym_off);
                        if (sym->st_name < tmp_str.size()){
                            const char* n = tmp_str.c_str()+sym->st_name;
                            r.sym_name = std::string(n);
                        }
                    }
                }
            }
            info.relocations.push_back(std::move(r));
        }
    };
    collect_rela(".rela.plt");
    collect_rela(".rela.dyn");
    // Also handle .rela.* generic sections on toolchains that emit that naming
    for(auto& sec: info.sections){
        if(sec.name.rfind(".rela.",0)==0 && sec.name!=".rela.plt" && sec.name!=".rela.dyn"){
            collect_rela(sec.name);
        }
    }

    if(info.symbols.empty() && info.text_bytes.empty()){
        // Not fatal: stripped binary with only dynsym is still usable for imports but not for function hooks.
        // Keep empty symbol list; caller stats will show raw/inlined counts.
    }

    return info;
}

uint64_t va_to_rva(const ElfInfo& info, uint64_t va){
    if (va < info.image_base_vaddr) return va;
    return va - info.image_base_vaddr;
}
uint64_t rva_to_va(const ElfInfo& info, uint64_t rva){
    return info.image_base_vaddr + rva;
}
std::string rva_to_section_name(const ElfInfo& info, uint64_t rva){
    uint64_t va = rva_to_va(info, rva);
    for(auto& s: info.sections){
        if(va >= s.virtual_address && va < s.virtual_address + s.size) return s.name;
    }
    return "";
}
uint64_t find_got_slot_va(const ElfInfo& info, const std::string& module, const std::string& symbol){
    std::string want_sym = to_lower_str(symbol);
    std::string want_mod = to_lower_str(module);
    for(auto& r: info.relocations){
        if(r.sym_name.empty()) continue;
        std::string cur = to_lower_str(r.sym_name);
        if(cur != want_sym) continue;
        // If module filter non-empty, try to match soname or rely on caller to have chosen correct ElfInfo.
        // Since we parse only one ELF (the main image), module filter is best-effort: if want_mod non-empty and soname doesn't contain it, still return slot (caller will validate via build_id).
        if(!want_mod.empty()){
            std::string soname_l = to_lower_str(info.soname);
            if(!soname_l.empty() && soname_l.find(want_mod)==std::string::npos){
                // keep searching but don't hard fail; allow match with warning
            }
        }
        if(r.type==R_X86_64_JUMP_SLOT || r.type==R_X86_64_GLOB_DAT || r.type==R_X86_64_64) return r.offset;
    }
    return 0;
}

} // namespace orca::hook_sdkgen
