#include "orca/patch2plugin/manifest.hpp"
#include <fstream>
#include <sstream>
#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <filesystem>
#ifdef HAVE_ZLIB
#include <zlib.h>
#endif

namespace orca::patch2plugin {

std::string normalize_path(const std::string& p) {
    std::string out;
    out.reserve(p.size());
    for (char c : p) {
        if (c == '\\') out.push_back('/');
        else out.push_back(c);
    }
    return out;
}
std::string basename_of(const std::string& p) {
    std::string n = normalize_path(p);
    auto pos = n.find_last_of('/');
    if (pos == std::string::npos) return n;
    return n.substr(pos + 1);
}
std::string json_escape(const std::string& s) {
    std::string out; out.reserve(s.size() + 8);
    for (unsigned char c : s) {
        if (c == '"') out += "\\\"";
        else if (c == '\\') out += "\\\\";
        else if (c == '\n') out += "\\n";
        else if (c == '\r') out += "\\r";
        else if (c == '\t') out += "\\t";
        else if (c < 0x20) { char b[7]; std::snprintf(b, sizeof(b), "\\u%04x", (int)c); out += b; }
        else out.push_back((char)c);
    }
    return out;
}

static std::string read_file_bytes(const std::string& path, bool &is_gz) {
    is_gz = path.size() >= 3 && path.substr(path.size() - 3) == ".gz";
    std::ifstream in(path, std::ios::binary);
    if (!in) throw std::runtime_error("cannot open manifest: " + path);
    std::string data((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    return data;
}

static std::string decompress_gz(const std::string& data) {
#ifdef HAVE_ZLIB
    z_stream strm{};
    strm.next_in = reinterpret_cast<Bytef*>(const_cast<char*>(data.data()));
    strm.avail_in = (uInt)data.size();
    if (inflateInit2(&strm, 47) != Z_OK) throw std::runtime_error("zlib inflateInit failed for manifest");
    std::string out;
    out.reserve(data.size() * 3);
    char buf[32768];
    int ret;
    do {
        strm.next_out = reinterpret_cast<Bytef*>(buf);
        strm.avail_out = sizeof(buf);
        ret = inflate(&strm, Z_NO_FLUSH);
        if (ret == Z_NEED_DICT || ret == Z_DATA_ERROR || ret == Z_MEM_ERROR) {
            std::string msg = strm.msg ? strm.msg : "unknown";
            inflateEnd(&strm);
            throw std::runtime_error("zlib inflate failed for manifest: " + msg + " (CRC/ISIZE mismatch indicates corrupted gzip)");
        }
        size_t have = sizeof(buf) - strm.avail_out;
        out.append(buf, have);
        if (ret == Z_BUF_ERROR) ret = Z_OK;
    } while (ret != Z_STREAM_END);
    inflateEnd(&strm);
    return out;
#else
    (void)data;
    throw std::runtime_error("manifest is gzipped but zlib not available");
#endif
}

static std::string extract_json_string(const std::string& json, const std::string& key) {
    std::string pat = "\"" + key + "\"";
    size_t pos = 0;
    while (true) {
        size_t p = json.find(pat, pos);
        if (p == std::string::npos) return "";
        size_t colon = json.find(':', p + pat.size());
        if (colon == std::string::npos) return "";
        size_t i = colon + 1;
        while (i < json.size() && std::isspace((unsigned char)json[i])) ++i;
        if (i >= json.size() || json[i] != '"') { pos = p + pat.size(); continue; }
        std::string out;
        ++i;
        while (i < json.size()) {
            char c = json[i];
            if (c == '\\') {
                if (i + 1 >= json.size()) break;
                char n = json[i + 1];
                if (n == '"') out.push_back('"');
                else if (n == '\\') out.push_back('\\');
                else if (n == 'n') out.push_back('\n');
                else if (n == 'r') out.push_back('\r');
                else if (n == 't') out.push_back('\t');
                else if (n == 'u') { // skip \uXXXX
                    if (i + 5 < json.size()) { out.push_back('?'); i += 5; }
                } else out.push_back(n);
                i += 2;
            } else if (c == '"') { ++i; break; }
            else { out.push_back(c); ++i; }
        }
        // Extract first occurrence after "\"build\"" for build fields.
        return out;
    }
}

static uint64_t extract_json_uint(const std::string& json, const std::string& key, size_t start_pos = 0) {
    std::string pat = "\"" + key + "\"";
    size_t p = json.find(pat, start_pos);
    if (p == std::string::npos) return 0;
    size_t colon = json.find(':', p + pat.size());
    if (colon == std::string::npos) return 0;
    size_t i = colon + 1;
    while (i < json.size() && std::isspace((unsigned char)json[i])) ++i;
    size_t j = i;
    while (j < json.size() && (std::isdigit((unsigned char)json[j]))) ++j;
    if (j == i) return 0;
    return std::stoull(json.substr(i, j - i));
}

ManifestInfo load_manifest(const std::string& path) {
    bool is_gz = false;
    std::string raw = read_file_bytes(path, is_gz);
    std::string json;
    if (is_gz) {
        json = decompress_gz(raw);
    } else {
        json = raw;
    }
    ManifestInfo info;
    size_t build_pos = json.find("\"build\"");
    if (build_pos != std::string::npos) {
        size_t sub = json.find("\"build_id\"", build_pos);
        if (sub != std::string::npos) {
            size_t colon = json.find(':', sub);
            if (colon != std::string::npos) {
                size_t i = colon + 1;
                while (i < json.size() && std::isspace((unsigned char)json[i])) ++i;
                if (i < json.size() && json[i] == '"') {
                    ++i;
                    std::string out;
                    while (i < json.size() && json[i] != '"') {
                        if (json[i] == '\\') { out.push_back(json[i+1]); i+=2; } else { out.push_back(json[i]); ++i; }
                    }
                    info.build_id = out;
                }
            }
        }
        info.os = extract_json_string(json.substr(build_pos, 800), "os");
        info.arch = extract_json_string(json.substr(build_pos, 800), "arch");
    }
    if (info.build_id.empty()) {
        // fallback search globally
        info.build_id = extract_json_string(json, "build_id");
    }
    if (info.build_id.empty()) throw std::runtime_error("manifest missing build_id");

    size_t sym_pos = json.find("\"symbols\"");
    if (sym_pos == std::string::npos) throw std::runtime_error("manifest missing symbols");
    size_t arr_start = json.find('[', sym_pos);
    if (arr_start == std::string::npos) throw std::runtime_error("manifest symbols not array");
    size_t pos = arr_start + 1;
    // Iterate objects by counting braces at top level
    int depth = 0;
    size_t obj_start = std::string::npos;
    for (size_t i = pos; i < json.size(); ++i) {
        char c = json[i];
        if (c == '{') {
            if (depth == 0) obj_start = i;
            ++depth;
        } else if (c == '}') {
            --depth;
            if (depth == 0 && obj_start != std::string::npos) {
                std::string obj = json.substr(obj_start, i - obj_start + 1);
                ManifestSymbol sym;
                sym.id = extract_json_string(obj, "id");
                if (sym.id.empty()) { obj_start = std::string::npos; continue; }
                sym.name = extract_json_string(obj, "name");
                if (sym.name.empty()) sym.name = sym.id;
                sym.display_name = extract_json_string(obj, "display_name");
                if (sym.display_name.empty()) sym.display_name = sym.name;
                sym.rva = extract_json_uint(obj, "rva");
                sym.size = (uint32_t)extract_json_uint(obj, "size");
                size_t src_pos = obj.find("\"source\"");
                if (src_pos != std::string::npos) {
                    sym.source_file = extract_json_string(obj.substr(src_pos, 600), "file");
                    size_t line_pos = obj.find("\"line\"", src_pos);
                    if (line_pos != std::string::npos && line_pos < obj_start + obj.size()) {
                        sym.source_line = (uint32_t)extract_json_uint(obj, "line", line_pos);
                    }
                }
                // fallback: if no source, keep empty
                info.symbols.push_back(std::move(sym));
                obj_start = std::string::npos;
            }
        }
        if (depth == 0 && c == ']' ) break;
    }
    // Ensure deterministic order: sort by id then rva
    std::sort(info.symbols.begin(), info.symbols.end(), [](const ManifestSymbol& a, const ManifestSymbol& b){
        if (a.id != b.id) return a.id < b.id;
        return a.rva < b.rva;
    });
    return info;
}

} // namespace orca::patch2plugin
