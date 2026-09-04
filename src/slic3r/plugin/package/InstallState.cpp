#include "InstallState.hpp"
#include "PluginMetadata.hpp"
#include "Hash.hpp"

#include <fstream>
#include <algorithm>
#include <cctype>
#include <set>
#include <nlohmann/json.hpp>
#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

namespace Slic3r::Plugin::Package {

bool write_install_state_file(const boost::filesystem::path &file, const InstallState &state, std::string &error)
{
    if (state.schema != INSTALL_STATE_SCHEMA_VERSION) {
        error = "install state schema must be 1";
        return false;
    }
    if (!is_safe_filename(state.artifact)) {
        error = "install state artifact must be safe base filename, got '" + state.artifact + "'";
        return false;
    }
    {
        std::string low = state.artifact;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c){ return std::tolower(c); });
        bool ok = (low.size() >= 4 && (low.rfind(".dll") == low.size() - 4 || low.rfind(".jar") == low.size() - 4)) ||
                  (low.size() >= 3 && low.rfind(".so") == low.size() - 3);
        if (!ok) {
            error = "install state artifact must end with .dll, .so or .jar";
            return false;
        }
    }
    if (state.hash.size() != 64 || !is_hex64(state.hash)) {
        error = "install state hash must be 64 hex chars";
        return false;
    }
    std::string hash_lower = state.hash;
    std::transform(hash_lower.begin(), hash_lower.end(), hash_lower.begin(), [](unsigned char c){ return std::tolower(c); });
    if (!is_valid_semver(state.version)) {
        error = "install state version must be valid semver, got '" + state.version + "'";
        return false;
    }

    nlohmann::json j;
    j["schema"]   = state.schema;
    j["artifact"] = state.artifact;
    j["hash"]     = hash_lower;
    j["version"]  = state.version;
    j["enabled"]  = state.enabled;

    boost::system::error_code ec;
    boost::filesystem::create_directories(file.parent_path(), ec);
    // Write via temp file and rename for atomicity.
    boost::filesystem::path tmp = file;
    tmp += ".tmp";
    {
        boost::nowide::ofstream out(tmp.string(), std::ios::binary | std::ios::trunc);
        if (!out) {
            error = "cannot open install state for writing: " + tmp.string();
            return false;
        }
        out << j.dump(2);
        if (!out) {
            error = "write failed for install state: " + tmp.string();
            return false;
        }
    }
    boost::filesystem::rename(tmp, file, ec);
    if (ec) {
        boost::filesystem::remove(file, ec);
        boost::filesystem::rename(tmp, file, ec);
        if (ec) {
            error = "rename install state failed: " + ec.message();
            return false;
        }
    }
    return true;
}

bool read_install_state_file(const boost::filesystem::path &file, InstallState &out, std::string &error)
{
    if (!boost::filesystem::exists(file) || !boost::filesystem::is_regular_file(file)) {
        error = "install state file not found: " + file.string();
        return false;
    }
    boost::system::error_code ec;
    auto sz = boost::filesystem::file_size(file, ec);
    if (!ec && sz > 16 * 1024) {
        error = "install state too large";
        return false;
    }
    boost::nowide::ifstream in(file.string(), std::ios::binary);
    if (!in) {
        error = "cannot open install state file: " + file.string();
        return false;
    }
    std::string text((std::istreambuf_iterator<char>(in)), std::istreambuf_iterator<char>());
    if (text.empty()) { error = "install state empty"; return false; }
    if (text.size() > 16*1024) { error = "install state too large"; return false; }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(text, nullptr, true, true);
        if (j.is_discarded() || !j.is_object()) { error = "install state is not a json object"; return false; }
    } catch (const std::exception &e) {
        error = std::string("install state json parse error: ") + e.what();
        return false;
    }
    static const std::set<std::string> allowed = {"schema","artifact","hash","version","enabled"};
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            error = "install state unknown field '" + it.key() + "'";
            return false;
        }
    }
    if (!j.contains("schema") || !j["schema"].is_number_integer()) { error = "'schema' must be integer"; return false; }
    int s = j["schema"].get<int>();
    if (s != INSTALL_STATE_SCHEMA_VERSION) { error = "'schema' must be 1"; return false; }
    if (!j.contains("artifact") || !j["artifact"].is_string()) { error = "'artifact' must be string"; return false; }
    if (!j.contains("hash") || !j["hash"].is_string()) { error = "'hash' must be string"; return false; }
    if (!j.contains("version") || !j["version"].is_string()) { error = "'version' must be string"; return false; }
    if (!j.contains("enabled") || !j["enabled"].is_boolean()) { error = "'enabled' must be boolean"; return false; }

    std::string artifact = j["artifact"].get<std::string>();
    std::string hash = j["hash"].get<std::string>();
    std::string version = j["version"].get<std::string>();
    bool enabled = j["enabled"].get<bool>();

    if (!is_safe_filename(artifact)) { error = "'artifact' must be safe base filename"; return false; }
    {
        std::string low = artifact;
        std::transform(low.begin(), low.end(), low.begin(), [](unsigned char c){ return std::tolower(c); });
        bool ok = (low.size() >= 4 && (low.rfind(".dll") == low.size() - 4 || low.rfind(".jar") == low.size() - 4)) ||
                  (low.size() >= 3 && low.rfind(".so") == low.size() - 3);
        if (!ok) { error = "'artifact' must end with .dll/.so/.jar"; return false; }
    }
    if (!is_hex64(hash)) { error = "'hash' must be 64 hexadecimal characters"; return false; }
    std::string hash_lower = hash;
    std::transform(hash_lower.begin(), hash_lower.end(), hash_lower.begin(),
                   [](unsigned char c) { return static_cast<char>(std::tolower(c)); });
    if (!is_valid_semver(version)) { error = "'version' must be valid semver"; return false; }
    {
        const std::vector<std::string> keys = {"schema","artifact","hash","version","enabled"};
        for (auto &k: keys) {
            std::string pat = "\"" + k + "\"";
            size_t first = text.find(pat);
            if (first != std::string::npos) {
                size_t second = text.find(pat, first+pat.size());
                if (second != std::string::npos) {
                    auto is_key = [&](size_t pos)->bool{
                        size_t after = pos+pat.size();
                        while (after<text.size() && std::isspace((unsigned char)text[after])) ++after;
                        return after<text.size() && text[after]==':';
                    };
                    if (is_key(first) && is_key(second)) { error = "duplicate key '" + k + "'"; return false; }
                }
            }
        }
    }

    out.schema = s;
    out.artifact = artifact;
    out.hash = hash_lower;
    out.version = version;
    out.enabled = enabled;
    return true;
}

bool write_install_state(const boost::filesystem::path &plugin_dir, const InstallState &state, std::string &error)
{
    boost::filesystem::path file = plugin_dir / INSTALL_STATE_FILENAME;
    return write_install_state_file(file, state, error);
}

bool read_install_state(const boost::filesystem::path &plugin_dir, InstallState &out, std::string &error)
{
    boost::filesystem::path file = plugin_dir / INSTALL_STATE_FILENAME;
    return read_install_state_file(file, out, error);
}

} // namespace Slic3r::Plugin::Package
