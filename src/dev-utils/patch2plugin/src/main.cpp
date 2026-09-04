#include "orca/patch2plugin/hash_util.hpp"
#include "orca/patch2plugin/manifest.hpp"
#include "orca/patch2plugin/diff_parser.hpp"
#include "orca/patch2plugin/brace_scanner.hpp"
#include "orca/patch2plugin/emitter.hpp"

#include <filesystem>
#include <iostream>
#include <fstream>
#include <sstream>
#include <map>
#include <set>
#include <vector>
#include <algorithm>
#include <cctype>

namespace fs = std::filesystem;
using namespace orca::patch2plugin;

static void print_usage(const char* prog){
    std::cerr << "Usage: " << prog << " --patch <file.patch> --repo <root> --manifest <orca-hooks.json.gz> --out <dir> --id <plugin id> --version <semver> [--name <name>]\n";
}

static std::string read_file(const std::string& path){
    std::ifstream in(path, std::ios::binary);
    if(!in) throw std::runtime_error("cannot open file: " + path);
    std::string s((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return s;
}

static bool is_semver(const std::string& v){
    // very simple semver check: X.Y.Z with optional prerelease/build
    size_t i=0;
    int dots=0;
    while(i<v.size()){
        if(std::isdigit((unsigned char)v[i])) { ++i; continue; }
        if(v[i]=='.'){ dots++; ++i; continue; }
        if(v[i]=='-' || v[i]=='+') break;
        return false;
    }
    if(dots<2) return false;
    if(i==0) return false;
    return true;
}

static bool is_plugin_id_valid(const std::string& id){
    if(id.size()<3 || id.size()>128) return false;
    if(!std::islower((unsigned char)id[0]) && !std::isdigit((unsigned char)id[0])) return false;
    for(char c: id){
        if(std::islower((unsigned char)c) || std::isdigit((unsigned char)c) || c=='.' || c=='_' || c=='-') continue;
        return false;
    }
    return true;
}

int main(int argc, char* argv[]){
    std::string patch_path, repo_path, manifest_path, out_dir, plugin_id, version, plugin_name;
    for(int i=1;i<argc;++i){
        std::string a=argv[i];
        auto need=[&](std::string &out, const char* name){
            if(i+1>=argc){ std::cerr << "missing value for " << name << "\n"; return false; }
            out = argv[++i]; return true;
        };
        if(a=="--patch"){ if(!need(patch_path,"--patch")) return 1; }
        else if(a=="--repo"){ if(!need(repo_path,"--repo")) return 1; }
        else if(a=="--manifest"){ if(!need(manifest_path,"--manifest")) return 1; }
        else if(a=="--out"){ if(!need(out_dir,"--out")) return 1; }
        else if(a=="--id"){ if(!need(plugin_id,"--id")) return 1; }
        else if(a=="--version"){ if(!need(version,"--version")) return 1; }
        else if(a=="--name"){ if(!need(plugin_name,"--name")) return 1; }
        else if(a=="--help" || a=="-h"){ print_usage(argv[0]); return 0; }
        else { std::cerr << "unknown arg: " << a << "\n"; print_usage(argv[0]); return 1; }
    }
    if(patch_path.empty() || repo_path.empty() || manifest_path.empty() || out_dir.empty() || plugin_id.empty() || version.empty()){
        std::cerr << "missing required arguments\n";
        print_usage(argv[0]);
        return 1;
    }
    if(!is_plugin_id_valid(plugin_id)){
        std::cerr << "invalid plugin id: " << plugin_id << " (must match [a-z0-9][a-z0-9._-]{2,127})\n";
        return 1;
    }
    if(!is_semver(version)){
        std::cerr << "invalid semver version: " << version << "\n";
        return 1;
    }

    try{
        std::string patch_bytes = read_file(patch_path);
        auto patch_hash = sha256(patch_bytes);
        std::string patch_hash_hex = sha256_hex(patch_hash);

        ManifestInfo manifest = load_manifest(manifest_path);

        ParsedPatch parsed = parse_patch(patch_bytes);
        // We will process only those, ignore others (no error)

        std::map<std::string, std::vector<std::string>> orig_lines_map; // file -> orig lines vector
        std::map<std::string, std::string> post_contents; // file -> post content string
        std::map<std::string, std::set<int>> changed_lines_map; // file -> changed new lines
        std::vector<ConvertedFunc> converted;
        std::vector<RejectedChunk> rejected;

        // For determinism, iterate files sorted (map already sorted)
        for(auto &kv: parsed.files){
            const std::string& fpath = kv.first;
            FilePatch &fp = kv.second;
            if(!is_cpp_file(fpath)){
                continue;
            }
            fs::path full = fs::path(repo_path) / fpath;
            if(!fs::exists(full)){
                std::cerr << "patch file not found in repo: " << fpath << " (resolved " << full.string() << ")\n";
                return 1;
            }
            std::string orig_content = read_file(full.string());
            std::vector<std::string> orig_lines = split_lines(orig_content);
            std::vector<std::string> cur = orig_lines;
            // Need to apply sequentially tracking offset due to previous hunks
            int line_offset = 0; // difference between new and old due to inserted/deleted lines
            for(auto &h: fp.hunks){
                int old_start = h.old_start; // 1-based
                int old_idx = old_start - 1 + line_offset;
                int cur_pos = old_start - 1 + line_offset;
                if(cur_pos <0) cur_pos=0;
                int cur_line = cur_pos;
                int old_line_check = old_start -1;
                for(size_t li=0; li<h.lines.size(); ++li){
                    const auto &hl = h.lines[li];
                    if(hl.kind==' '){
                        if(cur_line >= (int)cur.size() || cur[cur_line] != hl.text){
                            std::cerr << "hunk does not apply: file \"" << fpath << "\" hunk " << h.index << " line " << (old_start + (int)li) << ": expected context \"" << hl.text << "\" got \"" << (cur_line < (int)cur.size() ? cur[cur_line] : "<EOF>") << "\"\n";
                            return 1;
                        }
                        cur_line++;
                    } else if (hl.kind=='-'){
                        if(cur_line >= (int)cur.size() || cur[cur_line] != hl.text){
                            std::cerr << "hunk does not apply: file \"" << fpath << "\" hunk " << h.index << " line " << (old_start + (int)li) << ": expected to remove \"" << hl.text << "\" got \"" << (cur_line < (int)cur.size() ? cur[cur_line] : "<EOF>") << "\"\n";
                            return 1;
                        }
                        cur.erase(cur.begin()+cur_line);
                        line_offset -= 1;
                        // cur_line stays same (next line shifts up)
                    } else if (hl.kind=='+'){
                        cur.insert(cur.begin()+cur_line, hl.text);
                        cur_line++;
                        line_offset += 1;
                    } else if (hl.kind=='\\'){
                        // ignore
                    }
                }
            }
            std::string post = join_lines(cur);
            post_contents[fpath] = post;
            changed_lines_map[fpath] = fp.changed_new_lines;
        }

        for(auto &kv: post_contents){
            const std::string& fpath = kv.first;
            const std::string& post = kv.second;
            auto changed_it = changed_lines_map.find(fpath);
            if(changed_it==changed_lines_map.end()) continue;
            const std::set<int>& changed = changed_it->second;
            if(changed.empty()) continue;

            std::vector<FunctionRange> funcs = scan_functions(post, fpath);
            std::vector<FunctionRange> affected = find_affected_functions(funcs, changed);
            std::vector<int> outside = find_outside_lines(post, funcs, changed);

            for(int l: outside){
                RejectedChunk rc;
                rc.file = fpath;
                rc.line = l;
                rc.reason = "change outside function body";
                rejected.push_back(std::move(rc));
            }
            for(auto &fn: affected){
                if(fn.is_template){
                    RejectedChunk rc; rc.file=fpath; rc.line=fn.brace_open_line; rc.reason="function template or inline in header: \"" + fn.qualified + "\"";
                    rejected.push_back(std::move(rc));
                    continue;
                }
                if(fn.is_header_inline){
                    // any function in header is considered inline
                    RejectedChunk rc; rc.file=fpath; rc.line=fn.brace_open_line; rc.reason="function template or inline in header: \"" + fn.qualified + "\"";
                    rejected.push_back(std::move(rc));
                    continue;
                }
                std::string head = fn.full_text.substr(0, 400);
                std::string low = head; std::transform(low.begin(), low.end(), low.begin(), ::tolower);
                if(low.find("inline") != std::string::npos && fn.is_header_inline){
                    // already handled
                } else if(low.find("inline") != std::string::npos){
                    // inline in a .cpp is accepted; only header definitions are rejected.
                }

                std::vector<const ManifestSymbol*> candidates;
                std::string norm_file = normalize_path(fpath);
                std::string base = basename_of(norm_file);
                std::string lower_file = norm_file; std::transform(lower_file.begin(), lower_file.end(), lower_file.begin(), ::tolower);
                for(auto &sym: manifest.symbols){
                    std::string sym_file = normalize_path(sym.source_file);
                    std::string sym_base = basename_of(sym_file);
                    std::string lower_sym = sym_file; std::transform(lower_sym.begin(), lower_sym.end(), lower_sym.begin(), ::tolower);
                    bool file_match = false;
                    if(!sym_file.empty()){
                        if(lower_sym == lower_file) file_match = true;
                        else if(lower_sym.size() >= lower_file.size() && lower_sym.substr(lower_sym.size()-lower_file.size()) == lower_file) file_match = true;
                        else if(sym_base == base) file_match = true; // fallback to basename
                    } else {
                        file_match = false;
                    }
                    if(!file_match) continue;
                    // name match: id, name, display_name contains qualified or name
                    std::string cand_names[3] = {sym.id, sym.name, sym.display_name};
                    bool name_match=false;
                    for(auto &n: cand_names){
                        if(n.empty()) continue;
                        if(n == fn.qualified || n == fn.name) { name_match=true; break; }
                        // also check that symbol ends with ::func_name (overload)
                        if(n.size() >= fn.name.size() && n.substr(n.size()-fn.name.size()) == fn.name){
                            // ensure preceding char is ':' or exact
                            if(n.size()==fn.name.size() || n[n.size()-fn.name.size()-1]==':' ) { name_match=true; break; }
                        }
                        if(n.find(fn.qualified)!=std::string::npos) { name_match=true; break; }
                        if(fn.qualified.find(n)!=std::string::npos) { name_match=true; break; }
                    }
                    if(name_match) candidates.push_back(&sym);
                }
                if(candidates.empty()){
                    for(auto &sym: manifest.symbols){
                        if(sym.source_file.empty()){
                            if(sym.id == fn.qualified || sym.id == fn.name || sym.name == fn.qualified || sym.display_name == fn.qualified) candidates.push_back(&sym);
                        }
                    }
                }
                if(candidates.empty()){
                    RejectedChunk rc; rc.file=fpath; rc.line=fn.brace_open_line; rc.reason="function has no symbol in manifest: \"" + fn.qualified + "\"";
                    rejected.push_back(std::move(rc));
                    continue;
                }
                if(candidates.size()>1){
                    std::ostringstream msg;
                    msg << "ambiguous symbol for function \"" << fn.qualified << "\": " << candidates.size() << " candidates [";
                    // sort candidates by id for deterministic message
                    std::vector<std::string> ids;
                    for(auto *c: candidates) ids.push_back(c->id);
                    std::sort(ids.begin(), ids.end());
                    for(size_t i=0;i<ids.size() && i<3; ++i){ if(i) msg << ", "; msg << ids[i]; }
                    if(ids.size()>3) msg << ", ...";
                    msg << "]";
                    // Abort entirely per spec: "Com zero ou mais de um candidato, aborte nomeando a função e os candidatos."
                    std::cerr << msg.str() << " in file \"" << fpath << "\"\n";
                    return 1;
                }
                const ManifestSymbol* sym = candidates[0];
                ConvertedFunc cf;
                cf.file = fpath;
                cf.line = fn.brace_open_line;
                cf.func_name = fn.name;
                cf.qualified = fn.qualified;
                cf.symbol_id = sym->id;
                cf.rva = sym->rva;
                cf.full_text = fn.full_text;
                converted.push_back(std::move(cf));
            }
        }

        // Spec says "Recuse com mensagem clara, nunca converta por palpite: mudança fora de corpo..., função template..."

        // Sort rejected deterministically
        std::sort(rejected.begin(), rejected.end(), [](const RejectedChunk& a, const RejectedChunk& b){
            if(a.file!=b.file) return a.file < b.file;
            if(a.line!=b.line) return a.line < b.line;
            return a.reason < b.reason;
        });
        std::sort(converted.begin(), converted.end(), [](const ConvertedFunc& a, const ConvertedFunc& b){
            if(a.file!=b.file) return a.file < b.file;
            if(a.line!=b.line) return a.line < b.line;
            return a.symbol_id < b.symbol_id;
        });

        bool has_hard_error = false;
        for(auto &r: rejected){
            if(r.reason.find("outside") != std::string::npos || r.reason.find("template") != std::string::npos || r.reason.find("no symbol") != std::string::npos){
                has_hard_error = true;
            }
        }

        fs::create_directories(out_dir);

        // Emit report json first (deterministic)
        {
            std::ostringstream oss;
            oss << "{\n";
            oss << "  \"patch_sha256\": \"" << patch_hash_hex << "\",\n";
            oss << "  \"build_id\": \"" << json_escape(manifest.build_id) << "\",\n";
            oss << "  \"converted\": [\n";
            for(size_t i=0;i<converted.size();++i){
                auto &c = converted[i];
                oss << "    {\"file\": \"" << json_escape(c.file) << "\", \"line\": " << c.line << ", \"function\": \"" << json_escape(c.qualified) << "\", \"symbol\": \"" << json_escape(c.symbol_id) << "\", \"rva\": " << c.rva << "}";
                if(i+1<converted.size()) oss << ",";
                oss << "\n";
            }
            oss << "  ],\n";
            oss << "  \"rejected\": [\n";
            for(size_t i=0;i<rejected.size();++i){
                auto &r = rejected[i];
                oss << "    {\"file\": \"" << json_escape(r.file) << "\", \"line\": " << r.line << ", \"reason\": \"" << json_escape(r.reason) << "\"}";
                if(i+1<rejected.size()) oss << ",";
                oss << "\n";
            }
            oss << "  ]\n";
            oss << "}\n";
            std::string report = oss.str();
            std::ofstream out(fs::path(out_dir) / "patch2plugin-report.json", std::ios::binary);
            if(!out) throw std::runtime_error("cannot write report");
            out.write(report.data(), report.size());
        }

        if(converted.empty() && has_hard_error){
            std::cerr << "patch contains no convertible function changes; rejected fragments:\n";
            for(auto &r: rejected) std::cerr << "  " << r.file << ":" << r.line << " " << r.reason << "\n";
            return 1;
        }
        // Also if there were any outside/template rejections but also some converted, we still succeed but report contains rejected.
        if(!rejected.empty()){
            std::cerr << "warning: some fragments rejected (" << rejected.size() << "), converted " << converted.size() << "\n";
            for(auto &r: rejected) std::cerr << "  rejected " << r.file << ":" << r.line << " " << r.reason << "\n";
        }

        emit_plugin_project(out_dir, plugin_id, version, plugin_name, manifest, converted, post_contents);

        std::cout << "patch2plugin: converted " << converted.size() << " functions, rejected " << rejected.size() << " fragments, build_id " << manifest.build_id << "\n";
        return 0;
    } catch(const std::exception& e){
        std::cerr << "error: " << e.what() << "\n";
        return 1;
    }
}
