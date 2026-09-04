#include "orca/patch2plugin/brace_scanner.hpp"
#include <cctype>
#include <algorithm>
#include <sstream>

namespace orca::patch2plugin {

static bool is_ident_char(char c){ return std::isalnum((unsigned char)c) || c=='_' || c==':' ; }
static bool is_space(char c){ return c==' ' || c=='\t' || c=='\r'; }

std::vector<FunctionRange> scan_functions(const std::string& file_content, const std::string& file_path) {
    std::vector<std::string> lines;
    {
        std::string cur;
        for(char c: file_content){
            if(c=='\n'){ lines.push_back(cur); cur.clear(); }
            else cur.push_back(c);
        }
        lines.push_back(cur);
        if(!lines.empty() && lines.back().empty() && !file_content.empty() && file_content.back()=='\n') lines.pop_back();
    }
    struct Pos { int line; int col; };
    std::string text = file_content;
    std::vector<int> line_of_offset;
    line_of_offset.reserve(text.size()+1);
    int cur_line = 1;
    for(size_t i=0;i<text.size();++i){ line_of_offset.push_back(cur_line); if(text[i]=='\n') cur_line++; }
    line_of_offset.push_back(cur_line);

    bool in_string = false;
    bool in_char = false;
    bool in_block_comment = false;
    bool in_line_comment = false;
    bool escaped = false;
    bool in_preproc = false;
    bool preproc_continuation = false;

    struct BraceEntry {
        bool is_func = false;
        FunctionRange func;
        int open_line = 0;
    };
    std::vector<BraceEntry> stack;
    std::vector<FunctionRange> funcs;

    auto is_line_start = [&](size_t idx)->bool{
        if(idx==0) return true;
        return text[idx-1]=='\n';
    };

    for (size_t i=0;i<text.size();++i){
        char c = text[i];
        int line = line_of_offset[i];

        if (in_line_comment){
            if (c=='\n'){ in_line_comment=false; in_preproc=false; }
            continue;
        }
        if (in_block_comment){
            if (c=='*' && i+1<text.size() && text[i+1]=='/'){ in_block_comment=false; ++i; }
            continue;
        }
        if (in_string){
            if (escaped){ escaped=false; }
            else if (c=='\\') escaped=true;
            else if (c=='"') in_string=false;
            continue;
        }
        if (in_char){
            if (escaped){ escaped=false; }
            else if (c=='\\') escaped=true;
            else if (c=='\'') in_char=false;
            continue;
        }
        // preproc line handling: if in_preproc, ignore until newline (not counting continuation)
        if (in_preproc){
            if (c=='\n'){
                // Determine if line ends with backslash (ignoring trailing spaces)
                bool cont = false;
                size_t k = i;
                while(k>0 && (text[k-1]==' '||text[k-1]=='\t'||text[k-1]=='\r')) --k;
                if(k>0 && text[k-1]=='\\') cont=true;
                if(!cont) in_preproc=false;
            }
            continue;
        }
        // detect start of preproc: at line start, optional spaces then '#'
        if (is_line_start(i)){
            size_t j=i;
            while(j<text.size() && is_space(text[j])) ++j;
            if(j<text.size() && text[j]=='#'){
                in_preproc=true;
            }
        }
        if (in_preproc) continue;

        if (c=='/' && i+1<text.size()){
            if (text[i+1]=='/'){ in_line_comment=true; ++i; continue; }
            if (text[i+1]=='*'){ in_block_comment=true; ++i; continue; }
        }
        if (c=='"'){ in_string=true; escaped=false; continue; }
        if (c=='\''){ in_char=true; escaped=false; continue; }

        if (c=='{'){
            // Heuristic: look backwards for ')'
            bool is_func_candidate = false;
            std::string func_name;
            std::string qualified;
            int sig_start_line = line;
            bool has_template = false;

            int paren_depth=0;
            size_t j=i;
            bool found_paren=false;
            size_t paren_pos = std::string::npos;
            size_t k = i;
            while(k>0){
                --k;
                char pc = text[k];
                if(pc==' '||pc=='\t'||pc=='\n'||pc=='\r') continue;
                if(pc==')'){ paren_pos=k; found_paren=true; break; }
                // if we hit ';' '}' '{' before ')', then not a function
                if(pc==';' || pc=='}' || pc=='{' || pc=='#') break;
                if(std::isalnum((unsigned char)pc) || pc=='_' || pc==':' || pc=='&' || pc=='*') {
                    // continue searching
                    continue;
                }
                break;
            }
            if(found_paren){
                paren_depth=1;
                size_t p = paren_pos;
                while(p>0 && paren_depth>0){
                    --p;
                    char cc = text[p];
                    if(cc==')') paren_depth++;
                    else if(cc=='(') paren_depth--;
                }
                if(paren_depth==0){
                    size_t lparen = p;
                    size_t q = lparen;
                    while(q>0 && std::isspace((unsigned char)text[q-1])) --q;
                    size_t name_end = q;
                    size_t name_start = q;
                    while(name_start>0 && (std::isalnum((unsigned char)text[name_start-1]) || text[name_start-1]=='_' || text[name_start-1]==':')) --name_start;
                    if(name_start < name_end){
                        std::string cand = text.substr(name_start, name_end - name_start);
                        // Filter control keywords
                        std::string lower = cand;
                        std::transform(lower.begin(), lower.end(), lower.begin(), ::tolower);
                        if(lower!="if" && lower!="for" && lower!="while" && lower!="switch" && lower!="catch" && lower!="return" && lower!="sizeof" && lower!="decltype" && lower!="static_cast" && lower!="dynamic_cast" && lower!="reinterpret_cast" && lower!="const_cast" && lower!="new" && lower!="delete"){
                            // Additional check: ensure before name there isn't "class" "struct" "namespace" etc that would indicate not function
                            qualified = cand;
                            auto pos = cand.rfind("::");
                            if(pos!=std::string::npos) func_name = cand.substr(pos+2);
                            else func_name = cand;
                            // Scan backwards up to 200 chars for "template"
                            size_t look = name_start > 200 ? name_start-200 : 0;
                            std::string before = text.substr(look, name_start-look);
                            std::string low_before = before;
                            std::transform(low_before.begin(), low_before.end(), low_before.begin(), ::tolower);
                            if(low_before.find("template")!=std::string::npos){
                                has_template = true;
                            }
                            // Heuristic to determine start line: find beginning of declaration (search back to previous ';' '{' '}' or line start)
                            int sig_line = line_of_offset[name_start];
                            size_t sig_start = name_start;
                            while(sig_start>0 && text[sig_start-1]!='\n' && text[sig_start-1]!=';' && text[sig_start-1]!='}' && text[sig_start-1]!='{') --sig_start;
                            sig_start_line = line_of_offset[sig_start];
                            // Mark as func candidate if qualifiers between ')' and '{' are only allowed words
                            std::string between = text.substr(paren_pos+1, i - paren_pos -1);
                            // Lower and check not contains '=' (which would be assignment) and not contains ';'
                            bool ok = true;
                            for(char bc: between){
                                if(bc==';' || bc=='=') { ok=false; break; }
                            }
                            if(ok){
                                is_func_candidate = true;
                            }
                        }
                    }
                }
            }
            BraceEntry be;
            be.open_line = line;
            if(is_func_candidate){
                FunctionRange fr;
                fr.name = func_name;
                fr.qualified = qualified.empty()?func_name:qualified;
                fr.brace_open_line = line;
                fr.start_line = sig_start_line; // approximate
                fr.is_template = has_template;
                // header inline detection later via file extension
                if(file_path.size()>=2){
                    std::string lp=file_path;
                    std::transform(lp.begin(), lp.end(), lp.begin(), ::tolower);
                    if(lp.size()>=2 && (lp.substr(lp.size()-2)==".h" || lp.substr(lp.size()-4)==".hpp")) fr.is_header_inline=true;
                    // Any function defined in a header counts as inline; the caller rejects header functions.
                }
                fr.signature = "";
                be.is_func = true;
                be.func = fr;
            } else {
                be.is_func = false;
            }
            stack.push_back(std::move(be));
        } else if (c=='}'){
            if(!stack.empty()){
                BraceEntry be = stack.back(); stack.pop_back();
                if(be.is_func){
                    int close_line = line;
                    FunctionRange fr = be.func;
                    fr.brace_close_line = close_line;
                    fr.body_start_line = fr.brace_open_line;
                    fr.body_end_line = close_line;
                    size_t start_off = 0;
                    int l=1; size_t off=0;
                    for(; off<text.size() && l < fr.start_line; ++off){ if(text[off]=='\n') l++; }
                    start_off = off;
                    size_t end_off = 0;
                    l=1; off=0;
                    for(; off<text.size() && l <= close_line; ++off){ if(text[off]=='\n'){ if(l==close_line) { end_off=off+1; break; } l++; } if(l==close_line+1) { end_off=off; break; } }
                    if(end_off==0) end_off = text.size();
                    else if(end_off>text.size()) end_off=text.size();
                    fr.full_text = text.substr(start_off, end_off - start_off);
                    size_t bopen = text.find('{', start_off);
                    if(bopen!=std::string::npos && bopen < end_off) fr.body_text = text.substr(bopen, end_off - bopen);
                    funcs.push_back(std::move(fr));
                }
            }
        }
    }
    return funcs;
}

std::vector<FunctionRange> find_affected_functions(const std::vector<FunctionRange>& funcs, const std::set<int>& changed_lines){
    std::vector<FunctionRange> out;
    for(auto &f: funcs){
        for(int l: changed_lines){
            if(l >= f.start_line && l <= f.brace_close_line){
                // ensure l is inside braces, not just signature before brace
                if(l >= f.brace_open_line && l <= f.brace_close_line){
                    out.push_back(f);
                    break;
                }
                // So signature change before brace should be considered outside.
            }
        }
    }
    std::sort(out.begin(), out.end(), [](const FunctionRange& a, const FunctionRange& b){
        if(a.qualified != b.qualified) return a.qualified < b.qualified;
        return a.start_line < b.start_line;
    });
    out.erase(std::unique(out.begin(), out.end(), [](const FunctionRange& a, const FunctionRange& b){
        return a.qualified==b.qualified && a.start_line==b.start_line;
    }), out.end());
    return out;
}

std::vector<int> find_outside_lines(const std::string& file_content, const std::vector<FunctionRange>& funcs, const std::set<int>& changed_lines){
    std::vector<int> out;
    for(int l: changed_lines){
        bool inside=false;
        for(auto &f: funcs){
            if(l >= f.brace_open_line && l <= f.brace_close_line){ inside=true; break; }
        }
        if(!inside){
            out.push_back(l);
        }
    }
    std::sort(out.begin(), out.end());
    return out;
}

} // namespace orca::patch2plugin
