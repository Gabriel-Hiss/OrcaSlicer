#include "orca/patch2plugin/diff_parser.hpp"
#include <sstream>
#include <stdexcept>
#include <algorithm>
#include <cctype>

namespace orca::patch2plugin {

bool is_cpp_file(const std::string& path) {
    std::string p = path;
    std::transform(p.begin(), p.end(), p.begin(), [](unsigned char c){ return std::tolower(c); });
    if (p.size() >= 2 && p.substr(p.size()-2)==".c") return true;
    if (p.size() >= 3 && p.substr(p.size()-3)==".h") return true;
    if (p.size() >= 4 && (p.substr(p.size()-4)==".cc" || p.substr(p.size()-4)==".cpp" || p.substr(p.size()-4)==".hpp")) return true;
    return false;
}

std::vector<std::string> split_lines(const std::string& text) {
    std::vector<std::string> out;
    std::string cur;
    for (size_t i=0;i<text.size();++i){
        char c = text[i];
        if (c=='\n'){
            out.push_back(cur);
            cur.clear();
        } else if (c=='\r'){
            if (i+1<text.size() && text[i+1]=='\n') continue;
            out.push_back(cur);
            cur.clear();
        } else cur.push_back(c);
    }
    out.push_back(cur);
    if (!out.empty() && out.back().empty() && !text.empty() && text.back()=='\n') {
        out.pop_back();
    }
    return out;
}
std::string join_lines(const std::vector<std::string>& lines) {
    std::string out;
    for (size_t i=0;i<lines.size();++i){
        out += lines[i];
        if (i+1<lines.size()) out += "\n";
    }
    if (!lines.empty()) out += "\n";
    return out;
}

ParsedPatch parse_patch(const std::string& patch_text) {
    ParsedPatch pp;
    std::vector<std::string> lines = split_lines(patch_text);
    // split_lines drops the trailing newline; hunk parsing needs the raw lines with their prefixes.
    std::vector<std::string> raw;
    {
        std::string cur;
        for (char c: patch_text){
            if (c=='\n'){ raw.push_back(cur); cur.clear();}
            else if (c=='\r'){}
            else cur.push_back(c);
        }
        if (!cur.empty()) raw.push_back(cur);
    }
    std::string cur_file_orig, cur_file_new;
    FilePatch* cur_fp = nullptr;
    int hunk_idx = 0;
    for (size_t i=0;i<raw.size();++i){
        const std::string& l = raw[i];
        if (l.rfind("--- ",0)==0){
            cur_file_orig = l.substr(4);
            auto tab = cur_file_orig.find('\t');
            if (tab!=std::string::npos) cur_file_orig = cur_file_orig.substr(0,tab);
            if (cur_file_orig.rfind("a/",0)==0) cur_file_orig = cur_file_orig.substr(2);
        } else if (l.rfind("+++ ",0)==0){
            cur_file_new = l.substr(4);
            auto tab = cur_file_new.find('\t');
            if (tab!=std::string::npos) cur_file_new = cur_file_new.substr(0,tab);
            if (cur_file_new.rfind("b/",0)==0) cur_file_new = cur_file_new.substr(2);
            std::string norm = cur_file_new;
            for(char &c: norm) if(c=='\\') c='/';
            auto it = pp.files.find(norm);
            if (it==pp.files.end()){
                FilePatch fp;
                fp.path = norm;
                fp.orig_path = cur_file_orig;
                fp.new_path = cur_file_new;
                pp.files[norm] = std::move(fp);
                cur_fp = &pp.files[norm];
                hunk_idx = 0;
            } else {
                cur_fp = &it->second;
            }
        } else if (l.rfind("@@ ",0)==0 && cur_fp){
            Hunk h;
            h.header = l;
            h.index = ++hunk_idx;
            // parse @@ -old_start,old_count +new_start,new_count @@
            int o_s=0,o_c=1,n_s=0,n_c=1;
            // format: @@ -10,7 +10,7 @@
            size_t p1 = l.find('-');
            size_t p2 = l.find(' ', p1);
            std::string old_part = l.substr(p1+1, p2-p1-1);
            size_t p3 = l.find('+', p2);
            size_t p4 = l.find(' ', p3);
            if (p4==std::string::npos) p4 = l.find("@@", p3);
            std::string new_part = l.substr(p3+1, p4-p3-1);
            auto parse_rc=[&](const std::string& s, int &start, int &cnt){
                auto comma = s.find(',');
                if (comma==std::string::npos){ start = std::stoi(s); cnt=1; }
                else { start = std::stoi(s.substr(0,comma)); cnt = std::stoi(s.substr(comma+1)); }
            };
            try{
                parse_rc(old_part, o_s, o_c);
                parse_rc(new_part, n_s, n_c);
            } catch(...){
                throw std::runtime_error("invalid hunk header: " + l);
            }
            h.old_start = o_s;
            h.old_count = o_c;
            h.new_start = n_s;
            h.new_count = n_c;
            // collect following lines until next header or file
            size_t j = i+1;
            int new_line = n_s;
            for (; j<raw.size(); ++j){
                const std::string& ll = raw[j];
                if (ll.rfind("@@ ",0)==0) break;
                if (ll.rfind("--- ",0)==0) break;
                if (ll.rfind("+++ ",0)==0) break;
                if (ll.rfind("diff --git",0)==0) break;
                if (ll.empty()){
                    HunkLine hl; hl.kind=' '; hl.text="";
                    h.lines.push_back(std::move(hl));
                    new_line++;
                    continue;
                }
                char k = ll[0];
                if (k!=' ' && k!='-' && k!='+' && k!='\\'){
                    break;
                }
                if (k=='\\'){
                    HunkLine hl; hl.kind='\\'; hl.text=ll;
                    h.lines.push_back(std::move(hl));
                    continue;
                }
                HunkLine hl; hl.kind=k; hl.text = ll.substr(1);
                h.lines.push_back(std::move(hl));
                if (k=='+'){
                    cur_fp->changed_new_lines.insert(new_line);
                    new_line++;
                } else if (k==' '){
                    new_line++;
                } else if (k=='-'){
                    // no increment new
                }
            }
            cur_fp->hunks.push_back(std::move(h));
            i = j-1;
        }
    }
    // But we keep all files; caller will filter
    return pp;
}

} // namespace orca::patch2plugin
