#pragma once
#include <string>
#include <vector>
#include <set>

namespace orca::patch2plugin {

struct FunctionRange {
    std::string name; // extracted identifier (last token before '(')
    std::string qualified;
    std::string signature;
    int start_line = 0; // line of function signature start (1-based)
    int brace_open_line = 0;
    int brace_close_line = 0;
    int body_start_line = 0;
    int body_end_line = 0;
    std::string body_text;
    std::string full_text;
    bool is_template = false;
    bool is_header_inline = false;
    bool is_declaration = false; // ends with ';' not '{'
};

std::vector<FunctionRange> scan_functions(const std::string& file_content, const std::string& file_path);

std::vector<FunctionRange> find_affected_functions(
    const std::vector<FunctionRange>& funcs,
    const std::set<int>& changed_lines);

// collect lines outside any function that are not whitespace/comment/preprocessor
std::vector<int> find_outside_lines(
    const std::string& file_content,
    const std::vector<FunctionRange>& funcs,
    const std::set<int>& changed_lines);

} // namespace orca::patch2plugin
