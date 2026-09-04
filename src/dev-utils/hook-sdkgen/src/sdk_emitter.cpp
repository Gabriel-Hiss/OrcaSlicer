#include "orca/hook_sdkgen/sdk_emitter.hpp"
#include "orca/hook_sdkgen/hash_util.hpp"
#include "orca/hook_sdkgen/reader.hpp"
#include <filesystem>
#include <fstream>
#include <sstream>
#include <iostream>
#include <algorithm>
#include <cctype>
#include <vector>
#include <map>
namespace fs = std::filesystem;
using namespace orca::hook_sdkgen;

static std::string sanitize(const std::string& s) {
    std::string o;
    o.reserve(s.size());
    for (char c: s) {
        if (std::isalnum((unsigned char)c) || c=='_') o.push_back(c);
        else o.push_back('_');
    }
    if (!o.empty() && std::isdigit((unsigned char)o[0])) o = "_" + o;
    if (o.empty()) o = "_";
    return o;
}
static std::string escape_rust_string(const std::string& s){
    std::string out; out.reserve(s.size()*2);
    for(char c: s){ if(c=='\\') out += "\\\\"; else if(c=='"') out += "\\\""; else if(c=='\n') out += "\\n"; else if(c=='\r') out += "\\r"; else if(c=='\t') out += "\\t"; else out += c; }
    return out;
}
static std::string escape_cpp_string(const std::string& s){
    std::string out; out.reserve(s.size()*2);
    for(char c: s){ if(c=='\\') out += "\\\\"; else if(c=='"') out += "\\\""; else if(c=='\n') out += "\\n"; else if(c=='\r') out += "\\r"; else if(c=='\t') out += "\\t"; else out += c; }
    return out;
}
static void write_file(const fs::path& p, const std::string& content){
    fs::create_directories(p.parent_path());
    std::ofstream out(p, std::ios::binary);
    if(!out) throw GenerationError("cannot write "+p.string());
    out.write(content.data(), content.size());
}

static std::string read_template(const fs::path& base, const std::string& rel){
    fs::path f = base / rel;
    std::ifstream in(f, std::ios::binary);
    if(!in) return "";
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

static fs::path find_template_base(){
#ifdef ORCA_TEMPLATE_DIR
    fs::path cand = ORCA_TEMPLATE_DIR;
    if(fs::exists(cand)) return cand;
#endif
    fs::path cur = fs::current_path();
    for(int i=0;i<6;++i){
        fs::path t = cur / "sdk" / "plugin_v1" / "templates";
        if(fs::exists(t)) return t;
        if(cur.has_parent_path()) cur = cur.parent_path(); else break;
    }
    return fs::path("sdk/plugin_v1/templates");
}

static void copy_with_replace(const fs::path& src, const fs::path& dst, const std::map<std::string,std::string>& repl){
    std::ifstream in(src, std::ios::binary);
    if(!in) throw GenerationError("missing template "+src.string());
    std::string content((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    for(auto& kv: repl){
        std::string ph = kv.first;
        size_t pos=0;
        while((pos=content.find(ph,pos))!=std::string::npos){
            content.replace(pos, ph.size(), kv.second);
            pos+=kv.second.size();
        }
    }
    if(content.find("@ORCA_")!=std::string::npos){
        size_t p = content.find("@ORCA_");
        size_t q = content.find('@', p+1);
        if(p!=std::string::npos && q!=std::string::npos){
            std::string cand = content.substr(p, q-p+1);
            bool is_placeholder = cand.size()>2;
            for(size_t k=1;k+1<cand.size() && is_placeholder;++k) if(!std::isupper((unsigned char)cand[k]) && cand[k]!='_' && !std::isdigit((unsigned char)cand[k])) is_placeholder=false;
            if(is_placeholder) throw GenerationError("placeholder survives in "+src.string()+": "+cand);
        }
    }
    if(content.find("@GENERATED")!=std::string::npos){
        // Remove GENERATED markers deterministically
        std::string filtered;
        filtered.reserve(content.size());
        for(size_t i=0;i<content.size();++i){
            if(content.compare(i, 11, "@GENERATED@")==0){ i+=10; continue; }
            if(content.compare(i, 26, "@GENERATED_SYMBOL_ENTRIES@")==0){ i+=25; continue; }
            if(content.compare(i, 24, "@GENERATED_SYMBOL_DECLS@")==0){ i+=23; continue; }
            if(content.compare(i, 29, "@GENERATED_HOOK_REQUEST_FILL@")==0){ i+=28; continue; }
            if(content.compare(i, 26, "@GENERATED_HANDLE_CAPTURE@")==0){ i+=25; continue; }
            if(content.compare(i, 23, "@GENERATED_TRAMPOLINES@")==0){ i+=22; continue; }
            if(content.compare(i, 21, "@GENERATED_REGISTRATION@")==0){ i+=20; continue; }
            if(content.compare(i, 23, "@GENERATED_BUILD_ID@")==0){ i+=22; continue; }
            filtered.push_back(content[i]);
        }
        content.swap(filtered);
    }
    write_file(dst, content);
}

static uint32_t crc_table[256];
static bool crc_init = []{ for(uint32_t i=0;i<256;++i){ uint32_t c=i; for(int k=0;k<8;++k) c= c&1 ? 0xEDB88320u ^ (c>>1) : c>>1; crc_table[i]=c; } return true; }();
static uint32_t crc32(const uint8_t* data, size_t len){
    uint32_t c=0xFFFFFFFFu;
    for(size_t i=0;i<len;++i) c = crc_table[(c ^ data[i]) & 0xFF] ^ (c>>8);
    return c ^ 0xFFFFFFFFu;
}
static void w16(std::vector<uint8_t>& o, uint16_t v){ o.push_back(v&0xFF); o.push_back((v>>8)&0xFF); }
static void w32(std::vector<uint8_t>& o, uint32_t v){ for(int i=0;i<4;++i) o.push_back((v>>(i*8))&0xFF); }

static void emit_zip_deterministic(const fs::path& root, const fs::path& zip_path){
    std::vector<fs::path> files;
    for(auto& e: fs::recursive_directory_iterator(root)){
        if(e.is_regular_file()) files.push_back(e.path());
    }
    std::sort(files.begin(), files.end());
    std::vector<uint8_t> out;
    std::vector<std::vector<uint8_t>> central;
    std::vector<uint8_t> central_block;
    uint32_t offset=0;
    for(auto& fp: files){
        fs::path rel = fs::relative(fp, root);
        std::string name = rel.generic_string();
        std::ifstream in(fp, std::ios::binary);
        std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
        uint32_t crc = crc32(reinterpret_cast<const uint8_t*>(data.data()), data.size());
        // local header
        std::vector<uint8_t> lh;
        w32(lh, 0x04034b50);
        w16(lh, 20); w16(lh, 0); w16(lh, 0); w16(lh, 0); w16(lh, 0);
        w32(lh, crc); w32(lh, (uint32_t)data.size()); w32(lh, (uint32_t)data.size());
        w16(lh, (uint16_t)name.size()); w16(lh, 0);
        for(char c: name) lh.push_back((uint8_t)c);
        // data
        std::vector<uint8_t> entry = lh;
        entry.insert(entry.end(), data.begin(), data.end());
        // central header
        std::vector<uint8_t> ch;
        w32(ch, 0x02014b50); w16(ch, 20); w16(ch, 20); w16(ch, 0); w16(ch, 0); w16(ch, 0); w16(ch, 0);
        w32(ch, crc); w32(ch, (uint32_t)data.size()); w32(ch, (uint32_t)data.size());
        w16(ch, (uint16_t)name.size()); w16(ch,0); w16(ch,0); w16(ch,0); w16(ch,0); w32(ch,0); w32(ch, offset);
        for(char c: name) ch.push_back((uint8_t)c);
        out.insert(out.end(), entry.begin(), entry.end());
        central.push_back(ch);
        offset += (uint32_t)entry.size();
    }
    uint32_t central_offset = offset;
    uint32_t central_size=0;
    for(auto& c: central){ out.insert(out.end(), c.begin(), c.end()); central_size+=(uint32_t)c.size(); }
    // EOCD
    w32(out, 0x06054b50); w16(out,0); w16(out,0); w16(out,(uint16_t)central.size()); w16(out,(uint16_t)central.size());
    w32(out, central_size); w32(out, central_offset); w16(out,0);
    fs::create_directories(zip_path.parent_path());
    std::ofstream z(zip_path, std::ios::binary);
    z.write(reinterpret_cast<const char*>(out.data()), out.size());
}

void orca::hook_sdkgen::emit_cpp_sdk(const Manifest& m, const std::string& sdk_root){
    Manifest sm = m;
    sm.sort_deterministic();
    const Manifest& ms = sm;
    fs::path base = find_template_base();
    fs::path dst = fs::path(sdk_root) / "cpp";
    std::string build_id = ms.build.build_id.empty() ? "unknown" : ms.build.build_id;
    std::string os = os_to_string(ms.build.os);
    // symbols.hpp generation — symbols sorted by id lexicographically (deterministic)
    std::ostringstream sym;
    sym << "#pragma once\n#include \"orca/plugin/hook_target.hpp\"\n";
    sym << "static constexpr const char* ORCA_BUILD_ID = \"" << build_id << "\";\n";
    sym << "static constexpr uint32_t ORCA_HOOK_ABI = 1;\n";
    sym << "static constexpr const char* ORCA_OS = \"" << os << "\";\n";
    sym << "static constexpr const char* ORCA_ARCH = \"x86_64\";\n";
    sym << "namespace orca::symbols {\n";
    for(auto& s: ms.symbols){
        std::string var = sanitize(s.id);
        if(var.empty()) var = "sym_" + std::to_string(s.rva);
        std::string esc = escape_cpp_string(s.id);
        if(s.typed_binding.available){
            sym << "inline constexpr ::orca::plugin::HookTarget<void(*)()> " << var << "{\"" << esc << "\", " << s.rva << "};\n";
        } else {
            sym << "// raw: " << s.id << " rva=0x" << std::hex << s.rva << std::dec << " reason: " << s.typed_binding.reason << "\n";
            sym << "inline constexpr ::orca::plugin::HookTarget<void(*)()> " << var << "_raw{\"" << esc << "\", " << s.rva << "}; // raw\n";
        }
    }
    // borrowed handles for class/union types — types sorted by id (deterministic)
    sym << "// borrowed handles for non-trivial aggregates\n";
    for(auto& t: ms.types){
        if(t.kind==TypeKind::Class || t.kind==TypeKind::Union){
            std::string tn = sanitize(t.name);
            sym << "struct " << tn << "; using Borrowed_" << tn << " = ::orca::plugin::Borrowed<" << tn << ">;\n";
        }
    }
    sym << "}\n";
    std::vector<std::pair<std::string,std::string>> copies = {
        {"cpp/include/orca/plugin/hook.hpp","cpp/include/orca/plugin/hook.hpp"},
        {"cpp/include/orca/plugin/hook_target.hpp","cpp/include/orca/plugin/hook_target.hpp"},
        {"cpp/include/orca/plugin/raw.hpp","cpp/include/orca/plugin/raw.hpp"},
        {"cpp/include/orca/plugin/plugin.hpp","cpp/include/orca/plugin/plugin.hpp"},
        {"cpp/include/orca/plugin/detail/registry.hpp","cpp/include/orca/plugin/detail/registry.hpp"},
        {"cpp/include/orca/plugin/detail/cpu_context.hpp","cpp/include/orca/plugin/detail/cpu_context.hpp"},
    };
    std::map<std::string,std::string> repl = {{"@ORCA_BUILD_ID@", build_id},{"@ORCA_OS@", os},{"@ORCA_HOOK_ABI@", "1"}};
    for(auto& pr: copies){
        fs::path src = base / pr.first;
        fs::path d = dst / pr.second.substr(4); // strip "cpp/" prefix as dst already includes cpp
        std::string rel = pr.second.substr(4);
        copy_with_replace(src, dst / rel, {});
    }
    {
        fs::path src = base / "cpp/src/plugin_entry.cpp.in";
        std::map<std::string,std::string> r = {{"@ORCA_BUILD_ID@", build_id}};
        copy_with_replace(src, dst / "src/plugin_entry.cpp", r);
    }
    {
        fs::path src = base / "cpp/OrcaHookConfig.cmake.in";
        std::map<std::string,std::string> r = {{"@ORCA_BUILD_ID@", build_id},{"@ORCA_HOOK_ABI@", "1"},{"@PACKAGE_INIT@", "include(CMakeFindDependencyMacro)"}};
        copy_with_replace(src, dst / "lib/cmake/OrcaHook/OrcaHookConfig.cmake", r);
    }
    // metadata files with concrete example json — RC uses string type "ORCA_PLUGIN_METADATA" and "" escaping per MS RC rules
    std::string example_json = "{\"schema\":1,\"id\":\"com.orca.cpp-example\",\"name\":\"Cpp Example\",\"version\":\"0.1.0\",\"runtime\":\"native\",\"language\":\"cpp\",\"hook_abi\":1,\"targets\":[{\"os\":\""+os+"\",\"arch\":\"x86_64\",\"build_id\":\""+build_id+"\"}]}";
    {
        std::string escaped = example_json;
        // RC string: double each " to "" and keep \0 as literal inside same string
        std::string rc_escaped;
        rc_escaped.reserve(escaped.size()*2);
        for(char c: escaped){ if(c=='"') rc_escaped += "\"\""; else rc_escaped += c; }
        std::string rc = "#include <windows.h>\n1 \"ORCA_PLUGIN_METADATA\"\nBEGIN\n    \"" + rc_escaped + "\\0\"\nEND\n";
        write_file(dst / "src/metadata.rc", rc);
    }
    {
        std::string note = ".section \".note.orca.plugin\",\"a\",@note\n.balign 4\n.long 4\n.long " + std::to_string(example_json.size()+1) + "\n.long 0x4F524341\n.asciz \"ORCA\"\n.balign 4\n.asciz \"" + example_json + "\"\n.balign 4\n";
        write_file(dst / "src/metadata_note.S", note);
    }
    {
        std::string lds = "{\n  global:\n    orca_plugin_entry_v1;\n    orca_plugin_exit_v1;\n  local:\n    *;\n};\n";
        write_file(dst / "src/exports.lds", lds);
    }
    {
        std::string cmake = "cmake_minimum_required(VERSION 3.20)\nproject(orca_hook_plugin CXX)\nset(CMAKE_CXX_STANDARD 17)\nset(CMAKE_CXX_STANDARD_REQUIRED ON)\nfind_package(OrcaHook REQUIRED)\nadd_library(com_orca_cpp_example SHARED src/plugin_entry.cpp ../examples/cpp/src/plugin.cpp)\ntarget_include_directories(com_orca_cpp_example PRIVATE ${CMAKE_CURRENT_SOURCE_DIR}/include)\ntarget_link_libraries(com_orca_cpp_example PRIVATE OrcaHook::OrcaHook)\nif(WIN32)\nset_target_properties(com_orca_cpp_example PROPERTIES WINDOWS_EXPORT_ALL_SYMBOLS OFF)\nelse()\ntarget_link_options(com_orca_cpp_example PRIVATE \"LINKER:--version-script=${CMAKE_CURRENT_SOURCE_DIR}/src/exports.lds\")\nendif()\n";
        write_file(dst / "CMakeLists.txt", cmake);
    }
    // abi header copy — canonical source, must exist or build fails
    {
        fs::path src = base.parent_path() / "abi" / "orca_hook_api.h";
        if(!fs::exists(src)) {
            // Fallback: search upward from cwd for sdk/plugin_v1/abi/orca_hook_api.h (post-build cwd is build dir)
            fs::path cur = fs::current_path();
            for(int i=0;i<6;++i){
                fs::path cand = cur / "sdk" / "plugin_v1" / "abi" / "orca_hook_api.h";
                if(fs::exists(cand)){ src = cand; break; }
                if(cur.has_parent_path()) cur = cur.parent_path(); else break;
            }
        }
        if(!fs::exists(src)) src = fs::path("sdk/plugin_v1/abi/orca_hook_api.h");
        if(!fs::exists(src)) throw GenerationError("canonical abi header not found: sdk/plugin_v1/abi/orca_hook_api.h (tried " + src.string() + " and upward search)");
        copy_with_replace(src, dst / "abi/orca_hook_api.h", {});
        copy_with_replace(src, dst / "include/orca_hook_api.h", {});
        copy_with_replace(src, dst / "include/abi/orca_hook_api.h", {});
    }
}

void orca::hook_sdkgen::emit_rust_sdk(const Manifest& m, const std::string& sdk_root){
    Manifest sm = m;
    sm.sort_deterministic();
    const Manifest& ms = sm;
    fs::path base = find_template_base();
    fs::path dst = fs::path(sdk_root) / "rust";
    std::string build_id = ms.build.build_id.empty() ? "unknown" : ms.build.build_id;
    std::string os = os_to_string(ms.build.os);
    // generated.rs — symbols and types sorted lexicographically by id (deterministic), with dedup for sanitize collisions
    std::ostringstream gen;
    gen << "use crate::target::{HookPoint, HookTarget};\n";
    {
        std::map<std::string, bool> seen;
        for(auto& s: ms.symbols){
            std::string var = sanitize(s.id);
            if(var.empty()) var = "sym";
            std::string structName = var;
            structName[0]=std::toupper(structName[0]);
            std::string rawName = structName + "Raw";
            std::string key = s.typed_binding.available ? structName : rawName;
            if(seen.find(key)!=seen.end()) continue;
            seen[key]=true;
            if(s.typed_binding.available){
                std::string esc = escape_rust_string(s.id);
                gen << "pub struct " << structName << ";\n";
                gen << "impl HookTarget for " << structName << " { const SYMBOL_ID: &'static str = \"" << esc << "\"; const RVA: u32 = " << s.rva << "; const POINT: HookPoint = HookPoint::Entry; type Signature = fn(); }\n";
            } else {
                gen << "// raw: " << s.id << " rva=" << s.rva << " reason: " << s.typed_binding.reason << "\n";
                gen << "pub struct " << rawName << "; // raw\n";
            }
        }
        for(auto& t: ms.types){
            if(t.kind==TypeKind::Class || t.kind==TypeKind::Union){
                std::string tn = sanitize(t.name);
                std::string bh = "Borrowed_" + tn;
                if(seen.find(bh)!=seen.end()) continue;
                seen[bh]=true;
                gen << "pub struct " << bh << "(*const u8); // borrowed handle for " << t.name << "\n";
            }
        }
    }
    write_file(dst / "orca-hook" / "src" / "generated.rs", gen.str());
    {
        fs::path src = base / "rust/orca-hook/src/lib.rs";
        std::map<std::string,std::string> r = {{"@ORCA_BUILD_ID@", build_id}};
        copy_with_replace(src, dst / "orca-hook/src/lib.rs", r);
    }
    std::vector<std::string> copies = {"rust/orca-hook/src/hooks.rs","rust/orca-hook/src/target.rs","rust/orca-hook/src/handle.rs","rust/orca-hook/src/context.rs","rust/orca-hook/src/raw.rs","rust/orca-hook/Cargo.toml","rust/orca-hook-macros/Cargo.toml","rust/orca-hook-macros/src/lib.rs","rust/orca-hook-build/Cargo.toml","rust/orca-hook-build/src/lib.rs","rust/Cargo.toml"};
    for(auto& rel: copies){
        fs::path src = base / rel;
        if(rel=="rust/Cargo.toml"){
            std::map<std::string,std::string> r = {{"@ORCA_BUILD_ID@", build_id},{"@ORCA_HOOK_ABI@", "1"}};
            copy_with_replace(src, dst / "Cargo.toml", r);
        } else if(rel=="rust/orca-hook/src/generated.rs"){
            continue;
        } else {
            std::string dst_rel = rel.substr(5); // strip rust/
            copy_with_replace(src, dst / dst_rel, {});
        }
    }
    // example with priority 1200 already in template - copy
    {
        fs::path src = base / "rust/examples/example_plugin.rs";
        copy_with_replace(src, dst / "examples/example_plugin.rs", {});
    }
    {
        std::string cargo = "[package]\nname = \"rust-example\"\nversion = \"0.1.0\"\nedition = \"2021\"\n[lib]\ncrate-type = [\"cdylib\"]\n[dependencies]\norca-hook = { path = \"../orca-hook\" }\norca-hook-macros = { path = \"../orca-hook-macros\" }\n[build-dependencies]\norca-hook-build = { path = \"../orca-hook-build\" }\n";
        write_file(dst / "examples/Cargo.toml", cargo);
    }
}

void orca::hook_sdkgen::emit_jvm_sdk(const Manifest& m, const std::string& sdk_root){
    Manifest sm = m;
    sm.sort_deterministic();
    const Manifest& ms = sm;
    fs::path base = find_template_base();
    fs::path dst = fs::path(sdk_root) / "jvm";
    std::string build_id = ms.build.build_id.empty() ? "unknown" : ms.build.build_id;
    std::string os = os_to_string(ms.build.os);
    std::vector<std::string> java_files = {"jvm/src/main/java/org/orcaslicer/plugin/v1/Hook.java","jvm/src/main/java/org/orcaslicer/plugin/v1/Before.java","jvm/src/main/java/org/orcaslicer/plugin/v1/After.java","jvm/src/main/java/org/orcaslicer/plugin/v1/Replace.java","jvm/src/main/java/org/orcaslicer/plugin/v1/At.java","jvm/src/main/java/org/orcaslicer/plugin/v1/HookPoint.java","jvm/src/main/java/org/orcaslicer/plugin/v1/HookTarget.java","jvm/src/main/java/org/orcaslicer/plugin/v1/HookContext.java","jvm/src/main/java/org/orcaslicer/plugin/v1/Next.java","jvm/src/main/java/org/orcaslicer/plugin/v1/RawHook.java","jvm/src/main/java/org/orcaslicer/plugin/v1/CpuContext.java","jvm/src/main/java/org/orcaslicer/plugin/v1/OrcaHandle.java","jvm/src/main/java/org/orcaslicer/plugin/v1/NativeBridge.java","jvm/src/main/java/org/orcaslicer/plugin/v1/processor/HookProcessor.java","jvm/src/main/kotlin/org/orcaslicer/plugin/v1/OrcaHooksDsl.kt"};
    for(auto& rel: java_files){
        fs::path src = base / rel;
        std::string dst_rel = rel.substr(4); // strip jvm/
        copy_with_replace(src, dst / dst_rel, {});
    }
    {
        fs::path src = base / "jvm/src/main/resources/META-INF/services/javax.annotation.processing.Processor";
        copy_with_replace(src, dst / "src/main/resources/META-INF/services/javax.annotation.processing.Processor", {});
    }
    // GeneratedTargets.java with typed symbols — sorted lexicographically by id (deterministic)
    // Replace Windows backslashes to avoid illegal unicode escapes (\U) in Java comments/strings
    {
        std::ostringstream out;
        out << "package org.orcaslicer.plugin.v1.generated;\nimport org.orcaslicer.plugin.v1.*;\npublic final class GeneratedTargets {\n";
        for(auto& s: ms.symbols){
            std::string var = sanitize(s.id);
            std::string sid = s.id;
            std::string reason = s.typed_binding.reason;
            for(char &c: sid) if(c=='\\') c='/';
            for(char &c: reason) if(c=='\\') c='/';
            for(char &c: var) if(c=='\\') c='/';
            if(s.typed_binding.available){
                out << " public static final HookTarget " << var << " = new HookTarget(0, " << s.rva << "L, \"" << sid << "\");\n";
            } else {
                out << " // raw: " << sid << " rva=" << s.rva << " reason: " << reason << "\n";
            }
        }
        for(auto& t: ms.types){
            if(t.kind==TypeKind::Class || t.kind==TypeKind::Union){
                std::string tn = t.name;
                std::string vs = sanitize(tn);
                for(char &c: tn) if(c=='\\') c='/';
                for(char &c: vs) if(c=='\\') c='/';
                out << " public static final class Borrowed_" << vs << " extends OrcaHandle {} // " << tn << "\n";
            }
        }
        out << "}\n";
        std::string out_str = out.str();
        for(char &c: out_str) if(c=='\\') c='/';
        write_file(dst / "src/main/java/org/orcaslicer/plugin/v1/generated/GeneratedTargets.java", out_str);
    }
    {
        fs::path src = base / "jvm/build.gradle.kts.template";
        std::map<std::string,std::string> r = {{"9.7.1","9.7.1"}};
        copy_with_replace(src, dst / "build.gradle.kts", {});
    }
    {
        fs::path src = base / "jvm/settings.gradle.kts.template";
        copy_with_replace(src, dst / "settings.gradle.kts", {});
    }
    {
        fs::path src = base / "jvm/gradle.properties.template";
        copy_with_replace(src, dst / "gradle.properties", {});
    }
    {
        std::string props = "distributionBase=GRADLE_USER_HOME\ndistributionPath=wrapper/dists\ndistributionUrl=https\\://services.gradle.org/distributions/gradle-9.7.1-all.zip\nzipStoreBase=GRADLE_USER_HOME\nzipStorePath=wrapper/dists\n";
        write_file(dst / "gradle/wrapper/gradle-wrapper.properties", props);
        std::string jar_placeholder = "wrapper jar placeholder deterministic";
        write_file(dst / "gradle/wrapper/gradle-wrapper.jar", jar_placeholder);
        std::string sh = "#!/bin/sh\nexec gradle \"$@\"\n";
        write_file(dst / "gradlew", sh);
        std::string bat = "@echo off\ngradle %*\n";
        write_file(dst / "gradlew.bat", bat);
    }
    {
        std::string json = "{\"schema\":1,\"id\":\"com.orca.java-example\",\"name\":\"Java Example\",\"version\":\"0.1.0\",\"runtime\":\"jvm\",\"language\":\"java\",\"hook_abi\":1,\"targets\":[{\"os\":\""+os+"\",\"arch\":\"x86_64\",\"build_id\":\""+build_id+"\"}],\"entry_class\":\"org.orcaslicer.plugin.v1.examples.ExamplePlugin\"}";
        write_file(dst / "META-INF/orca/plugin.json", json);
        fs::path srcj = base / "jvm/examples/java/org/orcaslicer/plugin/v1/examples/ExamplePlugin.java";
        copy_with_replace(srcj, dst / "examples/java/org/orcaslicer/plugin/v1/examples/ExamplePlugin.java", {});
        fs::path srck = base / "jvm/examples/kotlin/org/orcaslicer/plugin/v1/examples/ExamplePluginKt.kt";
        copy_with_replace(srck, dst / "examples/kotlin/org/orcaslicer/plugin/v1/examples/ExamplePluginKt.kt", {});
    }
}

void orca::hook_sdkgen::emit_zip_and_checksum(const std::string& sdk_root, std::string* out_zip_path){
    fs::path root(sdk_root);
    fs::path zip = root.parent_path() / (root.filename().string() + ".zip");
    emit_zip_deterministic(root, zip);
    auto data = [&]{ std::ifstream f(zip, std::ios::binary); std::string s((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>()); return s; }();
    auto h = sha256(data);
    std::string hex = sha256_hex(h);
    fs::path chk = root.parent_path() / (root.filename().string() + ".sha256");
    write_file(chk, hex + "  " + zip.filename().string() + "\n");
    if(out_zip_path) *out_zip_path = zip.string();
}

void orca::hook_sdkgen::emit_sdks(const Manifest& m, const std::string& out_dir){
    std::string build_id = m.build.build_id.empty() ? "unknown" : m.build.build_id;
    fs::path out(out_dir);
    fs::path sdk_root;
    if(out.filename()=="hook-sdkgen") sdk_root = out.parent_path() / "plugin-sdk" / build_id;
    else if(out.filename()=="generated") sdk_root = out / "plugin-sdk" / build_id;
    else sdk_root = out / "plugin-sdk" / build_id;
    if(fs::exists(sdk_root)) fs::remove_all(sdk_root);
    fs::create_directories(sdk_root);
    emit_cpp_sdk(m, sdk_root.string());
    emit_rust_sdk(m, sdk_root.string());
    emit_jvm_sdk(m, sdk_root.string());
    emit_zip_and_checksum(sdk_root.string(), nullptr);
    // caller already emitted manifest/runtime to out_dir/manifest etc.
}
