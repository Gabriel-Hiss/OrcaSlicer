#include "orca/hook_sdkgen/dwarf_reader.hpp"
#include "orca/hook_sdkgen/elf_image.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/zydis_validator.hpp"

#include <algorithm>
#include <cstring>
#include <fstream>
#include <map>
#include <set>
#include <unordered_map>
#include <iostream>

#if __has_include(<elf.h>)
#  include <elf.h>
#endif

#if __has_include(<libdwarf/dwarf.h>)
#  include <libdwarf/dwarf.h>
#  include <libdwarf/libdwarf.h>
#  define HAVE_LIBDWARF 1
#elif __has_include(<dwarf.h>) && __has_include(<libdwarf.h>)
#  include <dwarf.h>
#  include <libdwarf.h>
#  define HAVE_LIBDWARF 1
#elif __has_include("dwarf.h")
#  include "dwarf.h"
#  define HAVE_LIBDWARF 1
#else
#  define HAVE_LIBDWARF 0
#endif

// Fallback for systems where STT_* comes only from <elf.h> and not dwarf headers
#ifndef STT_NOTYPE
#  define STT_NOTYPE 0
#endif
#ifndef STT_OBJECT
#  define STT_OBJECT 1
#endif
#ifndef STT_FUNC
#  define STT_FUNC 2
#endif

// DW_AT_varargs is not present in older libdwarf (e.g. 0.11 on Ubuntu resolute).
// Guard its use so the file compiles against both 0.11 and 2.3.2.
#ifndef DW_AT_varargs
#  ifdef DW_AT_prototyped
// If varargs attribute is missing, treat prototyped==false as crude varargs hint (unused strictly)
#    define ORCA_DW_AT_varargs DW_AT_prototyped
#    define ORCA_DW_AT_varargs_IS_PROTOTYPED 1
#  else
#    define ORCA_DW_AT_varargs 0
#    define ORCA_DW_AT_varargs_IS_PROTOTYPED 0
#  endif
#else
#  define ORCA_DW_AT_varargs_IS_PROTOTYPED 0
#endif

namespace orca::hook_sdkgen {

static TypedBinding classify_binding_linux(const std::string& type_name, TypeKind kind, bool is_complete, bool is_varargs, const std::string& callconv){
    TypedBinding tb{true,""};
    if (callconv=="unknown" || callconv=="varargs") { tb.available=false; tb.reason="unsupported calling convention: "+callconv; return tb; }
    if (is_varargs) { tb.available=false; tb.reason="varargs not supported"; return tb; }
    if (!is_complete) { tb.available=false; tb.reason="incomplete layout"; return tb; }
    switch(kind){
        case TypeKind::Void:
        case TypeKind::Bool:
        case TypeKind::I8: case TypeKind::U8: case TypeKind::I16: case TypeKind::U16:
        case TypeKind::I32: case TypeKind::U32: case TypeKind::I64: case TypeKind::U64:
        case TypeKind::F32: case TypeKind::F64:
        case TypeKind::Enum:
        case TypeKind::Pointer:
        case TypeKind::Reference:
        case TypeKind::Array:
            return tb;
        case TypeKind::Struct:
            return tb;
        case TypeKind::Class:
        case TypeKind::Union:
            tb.available=false; tb.reason="non-trivial class/union requires borrowed handle, not typed copy";
            return tb;
        case TypeKind::Function:
            tb.available=false; tb.reason="function type not directly bindable";
            return tb;
        default:
            tb.available=false; tb.reason="unsupported type kind: "+type_kind_to_string(kind);
            return tb;
    }
}

static BuildInfo buildinfo_from_elf(const ElfInfo& ei, const std::string& elf_path){
    BuildInfo b;
    b.os = Os::Linux;
    b.arch = Arch::X86_64;
    b.debug_file = elf_path;
    b.debug_age = 0;
    b.debug_guid = {};
    b.image_sha256 = ei.sha256;
    b.image_path = elf_path;
    std::string sha_hex = hex_bytes(ei.sha256);
    std::string short_sha = sha_hex.substr(0,12);
    std::string ghex = ei.gnu_build_id.hex;
    std::string short_g = ghex.empty() ? std::string(16,'0') : ghex.substr(0, std::min<size_t>(16, ghex.size()));
    b.build_id = std::string("linux-x86_64-") + short_g + "-" + short_sha;
    return b;
}

void DwarfReader::validate_elf_identity(const std::string& image_path, const std::string& debug_path, BuildInfo& out_build){
    std::string elf_for_build = image_path.empty() ? debug_path : image_path;
    if (elf_for_build.empty()) throw GenerationError("no ELF path supplied to DwarfReader");
    ElfInfo ei = read_elf_info(elf_for_build);
    out_build = buildinfo_from_elf(ei, elf_for_build);
    if (!image_path.empty() && !debug_path.empty() && image_path != debug_path){
        try{
            ElfInfo di = read_elf_info(debug_path);
            if (ei.gnu_build_id.found && di.gnu_build_id.found && ei.gnu_build_id.hex != di.gnu_build_id.hex){
                throw GenerationError("GNU build-id mismatch: image " + ei.gnu_build_id.hex + " != debug " + di.gnu_build_id.hex);
            }
        } catch(const GenerationError&){ throw; }
          catch(...){ }
    }
}

#if HAVE_LIBDWARF
static bool get_string_attr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attr, std::string& out){
    Dwarf_Attribute at=nullptr; Dwarf_Error err=nullptr;
    if (dwarf_attr(die, attr, &at, &err)!=DW_DLV_OK) return false;
    char* str=nullptr; Dwarf_Error e2=nullptr;
    if (dwarf_formstring(at, &str, &e2)!=DW_DLV_OK){ dwarf_dealloc(dbg, at, DW_DLA_ATTR); return false; }
    out = str ? str : "";
    dwarf_dealloc(dbg, str, DW_DLA_STRING);
    dwarf_dealloc(dbg, at, DW_DLA_ATTR);
    return true;
}
static bool get_flag_attr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attr, bool& out){
    Dwarf_Attribute at=nullptr; Dwarf_Error err=nullptr;
    if (dwarf_attr(die, attr, &at, &err)!=DW_DLV_OK) return false;
    Dwarf_Bool v=0;
    if (dwarf_formflag(at, &v, &err)!=DW_DLV_OK){ dwarf_dealloc(dbg, at, DW_DLA_ATTR); return false; }
    out = (v!=0);
    dwarf_dealloc(dbg, at, DW_DLA_ATTR);
    return true;
}
static bool get_uconst_attr(Dwarf_Debug dbg, Dwarf_Die die, Dwarf_Half attr, Dwarf_Unsigned& out){
    Dwarf_Attribute at=nullptr; Dwarf_Error err=nullptr;
    if (dwarf_attr(die, attr, &at, &err)!=DW_DLV_OK) return false;
    Dwarf_Unsigned v=0;
    if (dwarf_formudata(at, &v, &err)!=DW_DLV_OK){ dwarf_dealloc(dbg, at, DW_DLA_ATTR); return false; }
    out=v;
    dwarf_dealloc(dbg, at, DW_DLA_ATTR);
    return true;
}
#endif

Manifest DwarfReader::read(const std::string& image_path, const std::string& debug_path){
#if !HAVE_LIBDWARF
    (void)image_path; (void)debug_path;
    throw GenerationError("libdwarf 2.3.2 not available at build time; DwarfReader requires libdwarf headers and library (deps/libdwarf, SHA 7992e7b9019ebfabdda5773e86243517c48cf89fafed3209e853692bc9573efd). Build with -DHAVE_LIBDWARF or install libdwarf-dev. See plan 1.1/2.1.");
#else
    std::string elf_path = image_path.empty() ? debug_path : image_path;
    if (elf_path.empty()) throw GenerationError("DwarfReader: no image_path or debug_path supplied");
    BuildInfo build;
    validate_elf_identity(image_path, debug_path, build);
    ElfInfo ei = read_elf_info(elf_path);

    Manifest manifest;
    manifest.build = build;
    manifest.format_version = kFormatVersion;
    manifest.hook_abi = kHookAbiVersion;

    uint32_t next_type_id = 1;
    std::map<std::string,uint32_t> type_map;
    auto get_or_create_type=[&](const std::string& name, TypeKind kind, uint32_t sz, uint32_t align=0)->uint32_t{
        auto it=type_map.find(name);
        if(it!=type_map.end()) return it->second;
        TypeInfo ti; ti.id=next_type_id++; ti.name=name; ti.kind=kind; ti.size=sz; ti.align=align?align:sz; ti.is_complete=true; ti.is_trivially_copyable=true;
        manifest.types.push_back(std::move(ti));
        type_map[name]=manifest.types.back().id;
        return manifest.types.back().id;
    };
    get_or_create_type("void", TypeKind::Void, 0);
    get_or_create_type("bool", TypeKind::Bool, 1);
    get_or_create_type("int8_t", TypeKind::I8, 1);
    get_or_create_type("uint8_t", TypeKind::U8, 1);
    get_or_create_type("int16_t", TypeKind::I16, 2);
    get_or_create_type("uint16_t", TypeKind::U16, 2);
    get_or_create_type("int32_t", TypeKind::I32, 4);
    get_or_create_type("uint32_t", TypeKind::U32, 4);
    get_or_create_type("int64_t", TypeKind::I64, 8);
    get_or_create_type("uint64_t", TypeKind::U64, 8);
    get_or_create_type("float", TypeKind::F32, 4);
    get_or_create_type("double", TypeKind::F64, 8);

    Dwarf_Debug dbg=nullptr;
    Dwarf_Error err=nullptr;
    int res = dwarf_init_path(elf_path.c_str(), nullptr, 0, DW_GROUPNUMBER_ANY, nullptr, nullptr, &dbg, &err);
    if (res != DW_DLV_OK){
        std::string msg = "dwarf_init_path failed for " + elf_path;
        if(err) { char* em = dwarf_errmsg(err); if(em) msg += std::string(": ") + em; }
        if(dbg) dwarf_finish(dbg);
        throw GenerationError(msg + " -- ensure ELF was built with -g and not stripped before generation (plan 2.1).");
    }

    std::unordered_map<Dwarf_Off, uint32_t> die_to_type;

    // Signature (per libdwarf.h): header_length(Dwarf_Unsigned*), version(Dwarf_Half*), abbrev_offset(Dwarf_Off*),
    // address_size(Dwarf_Half*), length_size(Dwarf_Half*), extension_size(Dwarf_Half*), signature(Dwarf_Sig8*),
    // typeoffset(Dwarf_Unsigned*), next_offset(Dwarf_Unsigned*), header_cu_type(Dwarf_Half*), error(Dwarf_Error*)
    Dwarf_Unsigned cu_header_length = 0;
    Dwarf_Half     cu_version = 0;
    Dwarf_Off      cu_abbrev_offset = 0;
    Dwarf_Half     cu_address_size = 0;
    Dwarf_Half     cu_length_size = 0;
    Dwarf_Half     cu_extension_size = 0;
    Dwarf_Sig8     cu_signature{};
    Dwarf_Unsigned cu_typeoffset = 0;
    Dwarf_Unsigned cu_next_offset = 0;
    Dwarf_Half     cu_header_cu_type = 0;
    while (dwarf_next_cu_header_d(dbg, true,
            &cu_header_length, &cu_version, &cu_abbrev_offset,
            &cu_address_size, &cu_length_size, &cu_extension_size,
            &cu_signature, &cu_typeoffset, &cu_next_offset,
            &cu_header_cu_type, &err) == DW_DLV_OK){
        Dwarf_Die cu_die=nullptr;
        if (dwarf_siblingof_b(dbg, nullptr, true, &cu_die, &err)!=DW_DLV_OK) continue;
        std::vector<Dwarf_Die> stack; stack.push_back(cu_die);
        while(!stack.empty()){
            Dwarf_Die die = stack.back(); stack.pop_back();
            Dwarf_Half tag=0;
            if (dwarf_tag(die, &tag, &err)!=DW_DLV_OK){ dwarf_dealloc(dbg,die,DW_DLA_DIE); continue; }
            if(tag==DW_TAG_base_type || tag==DW_TAG_structure_type || tag==DW_TAG_class_type || tag==DW_TAG_union_type || tag==DW_TAG_enumeration_type || tag==DW_TAG_pointer_type || tag==DW_TAG_reference_type || tag==DW_TAG_array_type || tag==DW_TAG_typedef || tag==DW_TAG_subroutine_type){
                std::string tname;
                get_string_attr(dbg, die, DW_AT_name, tname);
                if(tname.empty()){
                    Dwarf_Off off=0; dwarf_dieoffset(die,&off,&err);
                    tname = std::string("anon_") + std::to_string((unsigned)tag) + "_" + std::to_string(off);
                }
                Dwarf_Unsigned byte_size=0; get_uconst_attr(dbg, die, DW_AT_byte_size, byte_size);
                TypeKind kind = TypeKind::Unknown;
                bool is_complete = true;
                if(tag==DW_TAG_base_type){
                    Dwarf_Unsigned enc=0;
                    if(get_uconst_attr(dbg, die, DW_AT_encoding, enc)){
                        switch(enc){
                            case DW_ATE_boolean: kind=TypeKind::Bool; break;
                            case DW_ATE_float: kind = (byte_size==4? TypeKind::F32 : TypeKind::F64); break;
                            case DW_ATE_signed: if(byte_size==1) kind=TypeKind::I8; else if(byte_size==2) kind=TypeKind::I16; else if(byte_size==4) kind=TypeKind::I32; else if(byte_size==8) kind=TypeKind::I64; break;
                            case DW_ATE_unsigned: if(byte_size==1) kind=TypeKind::U8; else if(byte_size==2) kind=TypeKind::U16; else if(byte_size==4) kind=TypeKind::U32; else if(byte_size==8) kind=TypeKind::U64; break;
                            case DW_ATE_signed_char: kind=TypeKind::I8; break;
                            case DW_ATE_unsigned_char: kind=TypeKind::U8; break;
                            default: kind=TypeKind::Unknown; break;
                        }
                    }
                } else if(tag==DW_TAG_pointer_type) kind=TypeKind::Pointer;
                  else if(tag==DW_TAG_reference_type) kind=TypeKind::Reference;
                  else if(tag==DW_TAG_array_type) kind=TypeKind::Array;
                  else if(tag==DW_TAG_structure_type) kind=TypeKind::Struct;
                  else if(tag==DW_TAG_class_type) kind=TypeKind::Class;
                  else if(tag==DW_TAG_union_type) kind=TypeKind::Union;
                  else if(tag==DW_TAG_enumeration_type) kind=TypeKind::Enum;
                  else if(tag==DW_TAG_subroutine_type) kind=TypeKind::Function;
                  else if(tag==DW_TAG_typedef) kind=TypeKind::Typedef;
                bool is_decl=false;
                if(get_flag_attr(dbg, die, DW_AT_declaration, is_decl) && is_decl) is_complete=false;
                std::string key = tname + "#" + std::to_string((int)kind) + "#" + std::to_string(byte_size);
                if(type_map.find(key)==type_map.end()){
                    TypeInfo ti; ti.id=next_type_id++; ti.name=tname; ti.kind=kind; ti.size=(uint32_t)byte_size; ti.align=(uint32_t)byte_size; ti.is_complete=is_complete; ti.is_trivially_copyable = (kind==TypeKind::Struct || kind==TypeKind::Enum || kind==TypeKind::Pointer);
                    if(kind==TypeKind::Enum){
                        Dwarf_Die child=nullptr;
                        if(dwarf_child(die,&child,&err)==DW_DLV_OK){
                            Dwarf_Die cur=child;
                            while(true){
                                Dwarf_Half ctag=0; dwarf_tag(cur,&ctag,&err);
                                if(ctag==DW_TAG_enumerator){
                                    std::string ename; get_string_attr(dbg,cur,DW_AT_name,ename);
                                    Dwarf_Unsigned eval=0; get_uconst_attr(dbg,cur,DW_AT_const_value,eval);
                                    ti.enum_values[(int64_t)eval]=ename;
                                }
                                Dwarf_Die sib=nullptr; int sres=dwarf_siblingof_b(dbg,cur,true,&sib,&err);
                                dwarf_dealloc(dbg,cur,DW_DLA_DIE);
                                if(sres!=DW_DLV_OK) break;
                                cur=sib;
                            }
                        }
                    }
                    if(kind==TypeKind::Struct || kind==TypeKind::Class || kind==TypeKind::Union){
                        Dwarf_Die child=nullptr;
                        if(dwarf_child(die,&child,&err)==DW_DLV_OK){
                            Dwarf_Die cur=child;
                            while(true){
                                Dwarf_Half ctag=0; dwarf_tag(cur,&ctag,&err);
                                if(ctag==DW_TAG_member){
                                    std::string fname; get_string_attr(dbg,cur,DW_AT_name,fname);
                                    Dwarf_Unsigned off=0; get_uconst_attr(dbg,cur,DW_AT_data_member_location,off);
                                    Dwarf_Attribute tat=nullptr; uint32_t tid=0;
                                    if(dwarf_attr(cur,DW_AT_type,&tat,&err)==DW_DLV_OK){
                                        Dwarf_Off toff=0; Dwarf_Bool is_info=true;
                                        if(dwarf_global_formref_b(tat,&toff,&is_info,&err)==DW_DLV_OK){
                                            tid = (uint32_t)toff;
                                        }
                                        dwarf_dealloc(dbg,tat,DW_DLA_ATTR);
                                    }
                                    TypeInfo::Field f; f.name=fname; f.offset=(uint32_t)off; f.type_id=tid;
                                    ti.fields.push_back(std::move(f));
                                }
                                Dwarf_Die sib=nullptr; int sres=dwarf_siblingof_b(dbg,cur,true,&sib,&err);
                                dwarf_dealloc(dbg,cur,DW_DLA_DIE);
                                if(sres!=DW_DLV_OK) break;
                                cur=sib;
                            }
                        }
                    }
                    manifest.types.push_back(std::move(ti));
                    type_map[key]=manifest.types.back().id;
                    Dwarf_Off off=0; dwarf_dieoffset(die,&off,&err);
                    die_to_type[off]=manifest.types.back().id;
                } else {
                    Dwarf_Off off=0; dwarf_dieoffset(die,&off,&err);
                    die_to_type[off]=type_map[key];
                }
            }
            Dwarf_Die child=nullptr;
            if(dwarf_child(die,&child,&err)==DW_DLV_OK){
                Dwarf_Die sib=nullptr;
                if(dwarf_siblingof_b(dbg,die,true,&sib,&err)==DW_DLV_OK) stack.push_back(sib);
                stack.push_back(child);
                dwarf_dealloc(dbg,die,DW_DLA_DIE);
                continue;
            }
            Dwarf_Die sib=nullptr;
            if(dwarf_siblingof_b(dbg,die,true,&sib,&err)==DW_DLV_OK) stack.push_back(sib);
            dwarf_dealloc(dbg,die,DW_DLA_DIE);
        }
    }

    struct DieSymInfo { std::string name; uint64_t low=0; uint64_t high=0; uint32_t type_off=0; std::string file; uint32_t line=0; std::string cc; bool is_varargs=false; bool is_decl=false; };
    std::unordered_map<uint64_t, DieSymInfo> pc_to_die;
    dwarf_finish(dbg);
    if (dwarf_init_path(elf_path.c_str(), nullptr, 0, DW_GROUPNUMBER_ANY, nullptr, nullptr, &dbg, &err)!=DW_DLV_OK){
        throw GenerationError("dwarf re-init failed for second pass on " + elf_path);
    }
    // Reset CU locals for second pass
    cu_header_length = 0; cu_version = 0; cu_abbrev_offset = 0; cu_address_size = 0; cu_length_size = 0; cu_extension_size = 0; cu_typeoffset = 0; cu_next_offset = 0; cu_header_cu_type = 0;
    while (dwarf_next_cu_header_d(dbg, true,
            &cu_header_length, &cu_version, &cu_abbrev_offset,
            &cu_address_size, &cu_length_size, &cu_extension_size,
            &cu_signature, &cu_typeoffset, &cu_next_offset,
            &cu_header_cu_type, &err) == DW_DLV_OK){
        Dwarf_Die cu_die=nullptr;
        if (dwarf_siblingof_b(dbg, nullptr, true, &cu_die, &err)!=DW_DLV_OK) continue;
        std::vector<Dwarf_Die> st; st.push_back(cu_die);
        while(!st.empty()){
            Dwarf_Die die=st.back(); st.pop_back();
            Dwarf_Half tag=0; dwarf_tag(die,&tag,&err);
            if(tag==DW_TAG_subprogram || tag==DW_TAG_variable){
                DieSymInfo info;
                get_string_attr(dbg,die,DW_AT_name,info.name);
                std::string link; if(get_string_attr(dbg,die,DW_AT_linkage_name,link) && !link.empty()) info.name=link;
                Dwarf_Unsigned low=0;
                Dwarf_Attribute at=nullptr;
                if(dwarf_attr(die,DW_AT_low_pc,&at,&err)==DW_DLV_OK){
                    Dwarf_Addr a=0; dwarf_formaddr(at,&a,&err); info.low=(uint64_t)a;
                    dwarf_dealloc(dbg,at,DW_DLA_ATTR);
                }
                if(dwarf_attr(die,DW_AT_high_pc,&at,&err)==DW_DLV_OK){
                    Dwarf_Half form=0; dwarf_whatform(at,&form,&err);
                    if(form==DW_FORM_addr){ Dwarf_Addr a=0; dwarf_formaddr(at,&a,&err); info.high=(uint64_t)a; }
                    else { Dwarf_Unsigned off=0; dwarf_formudata(at,&off,&err); info.high=info.low+off; }
                    dwarf_dealloc(dbg,at,DW_DLA_ATTR);
                }
                Dwarf_Attribute tat=nullptr;
                if(dwarf_attr(die,DW_AT_type,&tat,&err)==DW_DLV_OK){
                    Dwarf_Off off=0; Dwarf_Bool is_info=true;
                    if(dwarf_global_formref_b(tat,&off,&is_info,&err)==DW_DLV_OK) info.type_off=(uint32_t)off;
                    dwarf_dealloc(dbg,tat,DW_DLA_ATTR);
                }
                Dwarf_Unsigned lineno=0;
                if(get_uconst_attr(dbg,die,DW_AT_decl_line,lineno)) info.line=(uint32_t)lineno;
                std::string decl_file; get_string_attr(dbg,die,DW_AT_decl_file,decl_file); info.file=decl_file;
                Dwarf_Unsigned cc=0;
                if(get_uconst_attr(dbg,die,DW_AT_calling_convention,cc)){
                    if(cc==DW_CC_program) info.cc="program";
                    else if(cc==DW_CC_nocall) info.cc="nocall";
                    else info.cc="sysv";
                } else info.cc="sysv";
                Dwarf_Bool is_var=false;
#if defined(DW_AT_varargs)
                { bool b=false; if(get_flag_attr(dbg,die,DW_AT_varargs,b) && b) info.is_varargs=true; }
#elif ORCA_DW_AT_varargs_IS_PROTOTYPED
                // Fallback when DW_AT_varargs missing: treat non-prototyped as varargs hint
                { bool is_proto=false; if(get_flag_attr(dbg,die,ORCA_DW_AT_varargs,is_proto) && !is_proto) info.is_varargs=true; }
#else
                (void)is_var;
#endif
                bool is_decl=false; get_flag_attr(dbg,die,DW_AT_declaration,is_decl); info.is_decl=is_decl;
                if(info.low!=0) pc_to_die[info.low]=std::move(info);
            }
            Dwarf_Die child=nullptr;
            if(dwarf_child(die,&child,&err)==DW_DLV_OK){
                Dwarf_Die sib=nullptr;
                if(dwarf_siblingof_b(dbg,die,true,&sib,&err)==DW_DLV_OK) st.push_back(sib);
                st.push_back(child);
                dwarf_dealloc(dbg,die,DW_DLA_DIE);
                continue;
            }
            Dwarf_Die sib=nullptr;
            if(dwarf_siblingof_b(dbg,die,true,&sib,&err)==DW_DLV_OK) st.push_back(sib);
            dwarf_dealloc(dbg,die,DW_DLA_DIE);
        }
    }
    if(dbg) dwarf_finish(dbg);

    std::vector<Symbol> symbols;
    uint32_t inlined_no_addr=0;
    for(auto& es: ei.symbols){
        if(es.value==0){ inlined_no_addr++; continue; }
        if(es.type!=STT_FUNC && es.type!=STT_OBJECT) {
            if(es.type!=STT_NOTYPE || es.size==0) continue;
        }
        uint64_t rva = va_to_rva(ei, es.value);
        Symbol s;
        auto it = pc_to_die.find(es.value);
        if(it!=pc_to_die.end() && !it->second.name.empty()){
            s.id = it->second.name;
            s.decorated_name = it->second.name;
            s.display_name = es.name;
        } else {
            s.id = es.name;
            s.decorated_name = es.name;
            s.display_name = es.name;
        }
        s.rva = rva;
        s.size = (uint32_t)es.size;
        s.section = es.section_name.empty()? rva_to_section_name(ei, rva) : es.section_name;
        s.calling_convention = "sysv";
        if(es.type==STT_FUNC) s.kind=SymbolKind::Function;
        else if(es.type==STT_OBJECT) s.kind=SymbolKind::Data;
        else s.kind=SymbolKind::Data;
        if(it!=pc_to_die.end() && it->second.type_off!=0){
            auto f = die_to_type.find(it->second.type_off);
            if(f!=die_to_type.end()) s.type_id = f->second;
        }
        if(it!=pc_to_die.end() && (!it->second.file.empty() || it->second.line!=0)){
            s.source = SourceLocation{it->second.file, it->second.line, 0};
        }
        if(s.size>0) s.ranges.push_back(Range{(uint32_t)rva, s.size});
        bool in_text = (s.section==".text" && !ei.text_bytes.empty());
        if(in_text && s.kind==SymbolKind::Function){
            uint64_t text_va=0; for(auto& sec: ei.sections) if(sec.name==".text"){ text_va=sec.virtual_address; break; }
            uint64_t off_in_text = es.value >= text_va ? es.value - text_va : 0;
            if(off_in_text < ei.text_bytes.size()){
                size_t avail = std::min<size_t>(s.size? s.size : 64, ei.text_bytes.size()- (size_t)off_in_text);
                std::string err2;
                auto instrs = validate_instructions(ei.text_bytes.data()+off_in_text, avail, (uint32_t)rva, &err2);
                s.instructions = instrs;
            }
        }
        TypeKind tk = TypeKind::Unknown;
        std::string tname;
        bool is_complete=true, is_varargs=false;
        std::string cc="sysv";
        if(s.type_id!=0){
            for(auto& t: manifest.types) if(t.id==s.type_id){ tk=t.kind; tname=t.name; is_complete=t.is_complete; break; }
        }
        if(it!=pc_to_die.end()){
            cc = it->second.cc;
            is_varargs = it->second.is_varargs;
            if(it->second.is_decl) is_complete=false;
        }
        if(s.type_id==0){
            s.typed_binding = {false, "no DWARF type; raw handle"};
        } else {
            s.typed_binding = classify_binding_linux(tname, tk, is_complete, is_varargs, cc);
        }
        symbols.push_back(std::move(s));
    }
    for(auto& rel: ei.relocations){
        if(rel.sym_name.empty()) continue;
        Symbol s;
        s.id = rel.sym_name + "@GOT";
        s.decorated_name = rel.sym_name;
        s.display_name = rel.sym_name;
        s.kind = SymbolKind::Import;
        s.rva = va_to_rva(ei, rel.offset);
        s.size = 8;
        s.section = ".got.plt";
        s.calling_convention = "sysv";
        s.typed_binding = {false, "import symbol; raw GOT pointer"};
        bool dup=false;
        for(auto& ex: symbols) if(ex.id==s.id && ex.rva==s.rva){ dup=true; break; }
        if(!dup) symbols.push_back(std::move(s));
    }
    for(auto& es: ei.symbols){
        if(es.name.rfind("_ZTV",0)==0){
            for(auto& sym: symbols) if(sym.id==es.name){ sym.kind=SymbolKind::VTable; sym.typed_binding={false,"vtable requires borrowed handle"}; break; }
        }
    }
    manifest.symbols = std::move(symbols);
    manifest.stats.inlined_or_no_address = inlined_no_addr;
    manifest.sort_deterministic();
    manifest.recompute_stats();
    manifest.stats.inlined_or_no_address = inlined_no_addr;
    return manifest;
#endif
}

} // namespace orca::hook_sdkgen
