#pragma once
#include <string>
#include <vector>
#include <map>
#include <set>

namespace orca::patch2plugin {

struct HunkLine {
    char kind; // ' ', '-', '+', '\\' for no newline
    std::string text; // without prefix
};

struct Hunk {
    int old_start = 0;
    int old_count = 0;
    int new_start = 0;
    int new_count = 0;
    std::vector<HunkLine> lines;
    std::string header;
    int index = 0; // 1-based per file
};

struct FilePatch {
    std::string path; // normalized with '/'
    std::string orig_path; // a/...
    std::string new_path; // b/...
    std::vector<Hunk> hunks;
    std::set<int> changed_new_lines; // line numbers in post-image that are '+'
};

struct ParsedPatch {
    std::map<std::string, FilePatch> files; // key = normalized new_path without a/b prefix
};

ParsedPatch parse_patch(const std::string& patch_text);
bool is_cpp_file(const std::string& path);

std::vector<std::string> split_lines(const std::string& text);
std::string join_lines(const std::vector<std::string>& lines);

} // namespace orca::patch2plugin
