#include "PluginMetadata.hpp"

#include <algorithm>
#include <cctype>
#include <regex>
#include <set>
#include <sstream>

#include <nlohmann/json.hpp>

#include "../../../libslic3r/Semver.hpp"

namespace Slic3r::Plugin::Package {

namespace {

bool is_string_nonempty_trimmed(const std::string &s)
{
    if (s.empty()) return false;
    auto not_space = [](unsigned char c) { return !std::isspace(c); };
    return std::find_if(s.begin(), s.end(), not_space) != s.end();
}

} // namespace

bool is_valid_plugin_id(const std::string &id)
{
    if (id.empty()) return false;
    if (id.size() < 3 || id.size() > 128) return false;
    static const std::regex re("^[a-z0-9][a-z0-9._-]{2,127}$");
    if (!std::regex_match(id, re)) return false;
    if (id.find('/') != std::string::npos || id.find('\\') != std::string::npos) return false;
    // Reject ".." to block path traversal.
    if (id.find("..") != std::string::npos) {
        return false;
    }
    return true;
}

bool is_valid_semver(const std::string &version, std::string *normalized)
{
    if (version.empty()) return false;
    if (version.size() > 128) return false;
    static const std::regex re(R"(^(0|[1-9]\d*)\.(0|[1-9]\d*)\.(0|[1-9]\d*)(?:-((?:0|[1-9]\d*|\d*[a-zA-Z-][0-9A-Za-z-]*)(?:\.(?:0|[1-9]\d*|\d*[a-zA-Z-][0-9A-Za-z-]*))*))?(?:\+([0-9A-Za-z-]+(?:\.[0-9A-Za-z-]+)*))?$)");
    if (!std::regex_match(version, re)) return false;
    auto parsed = Slic3r::Semver::parse(version);
    if (!parsed) return false;
    if (normalized) {
        *normalized = version;
    }
    return true;
}

bool is_safe_filename(const std::string &filename)
{
    if (filename.empty()) return false;
    if (filename.find('/') != std::string::npos) return false;
    if (filename.find('\\') != std::string::npos) return false;
    if (filename.find('\0') != std::string::npos) return false;
    if (filename == "." || filename == "..") return false;
    if (filename.rfind("..", 0) == 0) return false;
    // Reject Windows drive letters.
    if (filename.find(':') != std::string::npos) return false;
    // Reject control characters.
    for (unsigned char c : filename) if (c < 0x20) return false;
    return true;
}

bool is_safe_path_component(const std::string &component)
{
    return is_valid_plugin_id(component); // plugin id is used as directory name
}

bool is_runtime_language_compatible(const PluginMetadata &meta, std::string &error)
{
    if (meta.runtime == "native") {
        if (meta.language != "cpp" && meta.language != "rust") {
            error = "runtime 'native' requires language 'cpp' or 'rust', got '" + meta.language + "'";
            return false;
        }
        if (meta.entry_class.has_value()) {
            error = "entry_class must not be present for native runtime";
            return false;
        }
    } else if (meta.runtime == "jvm") {
        if (meta.language != "java" && meta.language != "kotlin") {
            error = "runtime 'jvm' requires language 'java' or 'kotlin', got '" + meta.language + "'";
            return false;
        }
        if (!meta.entry_class.has_value() || meta.entry_class->empty()) {
            error = "entry_class is required for jvm runtime";
            return false;
        }
        const std::string &ec = *meta.entry_class;
        if (ec.find('/') != std::string::npos || ec.find('\\') != std::string::npos) {
            error = "entry_class must be dot-separated, not slash-separated";
            return false;
        }
        if (ec.empty() || ec.front() == '.' || ec.back() == '.') {
            error = "entry_class has leading/trailing dot";
            return false;
        }
    } else {
        error = "unknown runtime '" + meta.runtime + "'";
        return false;
    }
    return true;
}

bool validate_plugin_metadata_json(const nlohmann::json &j, PluginMetadata &out, std::string &error)
{
    auto fail = [&](const std::string &msg) -> bool {
        error = msg;
        return false;
    };

    if (!j.is_object()) return fail("metadata root must be an object");

    // The schema is strict; reject unknown fields.
    static const std::set<std::string> allowed = {
        "schema", "id", "name", "version", "runtime", "language",
        "hook_abi", "targets", "entry_class", "description", "author"
    };
    for (auto it = j.begin(); it != j.end(); ++it) {
        if (allowed.find(it.key()) == allowed.end()) {
            return fail("unknown field '" + it.key() + "'");
        }
    }

    if (!j.contains("schema")) return fail("missing required field 'schema'");
    if (!j["schema"].is_number_integer()) return fail("'schema' must be integer");
    int schema = j["schema"].get<int>();
    if (schema != PLUGIN_METADATA_SCHEMA_VERSION) {
        return fail("'schema' must be " + std::to_string(PLUGIN_METADATA_SCHEMA_VERSION));
    }

    if (!j.contains("id")) return fail("missing required field 'id'");
    if (!j["id"].is_string()) return fail("'id' must be string");
    std::string id = j["id"].get<std::string>();
    if (!is_valid_plugin_id(id)) {
        return fail("'id' must match [a-z0-9][a-z0-9._-]{2,127} and not contain path traversal (got '" + id + "')");
    }

    if (!j.contains("name")) return fail("missing required field 'name'");
    if (!j["name"].is_string()) return fail("'name' must be string");
    std::string name = j["name"].get<std::string>();
    if (!is_string_nonempty_trimmed(name)) return fail("'name' must be non-empty");
    if (name.size() > 256) return fail("'name' too long (max 256)");

    if (!j.contains("version")) return fail("missing required field 'version'");
    if (!j["version"].is_string()) return fail("'version' must be string");
    std::string version = j["version"].get<std::string>();
    std::string norm;
    if (!is_valid_semver(version, &norm)) return fail("'version' must be valid semver (got '" + version + "')");
    if (version.size() > 128) return fail("'version' too long");

    if (!j.contains("runtime")) return fail("missing required field 'runtime'");
    if (!j["runtime"].is_string()) return fail("'runtime' must be string");
    std::string runtime = j["runtime"].get<std::string>();
    if (runtime != "native" && runtime != "jvm") return fail("'runtime' must be 'native' or 'jvm'");

    if (!j.contains("language")) return fail("missing required field 'language'");
    if (!j["language"].is_string()) return fail("'language' must be string");
    std::string language = j["language"].get<std::string>();
    if (language != "cpp" && language != "rust" && language != "java" && language != "kotlin") {
        return fail("'language' must be one of cpp|rust|java|kotlin");
    }

    if (!j.contains("hook_abi")) return fail("missing required field 'hook_abi'");
    if (!j["hook_abi"].is_number_integer()) return fail("'hook_abi' must be integer");
    int hook_abi = j["hook_abi"].get<int>();
    if (hook_abi != PLUGIN_HOOK_ABI_VERSION) {
        return fail("'hook_abi' must be " + std::to_string(PLUGIN_HOOK_ABI_VERSION));
    }

    if (!j.contains("targets")) return fail("missing required field 'targets'");
    if (!j["targets"].is_array()) return fail("'targets' must be array");
    if (j["targets"].empty()) return fail("'targets' must contain at least one entry");
    if (j["targets"].size() > 16) return fail("'targets' too many entries (max 16)");

    std::vector<PluginTarget> targets;
    targets.reserve(j["targets"].size());
    std::set<std::string> seen_build_ids;
    for (size_t i = 0; i < j["targets"].size(); ++i) {
        const auto &t = j["targets"][i];
        std::string prefix = "'targets[" + std::to_string(i) + "]'";
        if (!t.is_object()) return fail(prefix + " must be object");
            static const std::set<std::string> allowed_t = {"os", "arch", "build_id"};
        for (auto it = t.begin(); it != t.end(); ++it) {
            if (allowed_t.find(it.key()) == allowed_t.end())
                return fail(prefix + " unknown field '" + it.key() + "'");
        }
        if (!t.contains("os") || !t["os"].is_string()) return fail(prefix + ".os must be string");
        if (!t.contains("arch") || !t["arch"].is_string()) return fail(prefix + ".arch must be string");
        if (!t.contains("build_id") || !t["build_id"].is_string()) return fail(prefix + ".build_id must be string");
        std::string os = t["os"].get<std::string>();
        std::string arch = t["arch"].get<std::string>();
        std::string build_id = t["build_id"].get<std::string>();
        if (os != "windows" && os != "linux") return fail(prefix + ".os must be windows|linux");
        if (arch != "x86_64") return fail(prefix + ".arch must be x86_64");
        if (build_id.empty()) return fail(prefix + ".build_id must be non-empty");
        if (build_id.size() > 256) return fail(prefix + ".build_id too long");
        if (build_id.find('/') != std::string::npos || build_id.find('\\') != std::string::npos ||
            build_id.find('\0') != std::string::npos) {
            return fail(prefix + ".build_id must not contain path separators");
        }
        if (build_id.find("..") != std::string::npos) return fail(prefix + ".build_id must not contain '..'");
        if (build_id.find_first_of(" \t\r\n") != std::string::npos) return fail(prefix + ".build_id must not contain whitespace");
        if (!seen_build_ids.insert(build_id).second) {
            return fail(prefix + ".build_id duplicate '" + build_id + "'");
        }
        targets.push_back({os, arch, build_id});
    }

    std::optional<std::string> entry_class;
    if (j.contains("entry_class")) {
        if (!j["entry_class"].is_string()) return fail("'entry_class' must be string");
        entry_class = j["entry_class"].get<std::string>();
        if (entry_class->empty()) return fail("'entry_class' must be non-empty if present");
        if (entry_class->size() > 512) return fail("'entry_class' too long");
    }

    std::optional<std::string> description;
    if (j.contains("description")) {
        if (!j["description"].is_string()) return fail("'description' must be string");
        description = j["description"].get<std::string>();
        if (description->size() > 2048) return fail("'description' too long");
    }
    std::optional<std::string> author;
    if (j.contains("author")) {
        if (!j["author"].is_string()) return fail("'author' must be string");
        author = j["author"].get<std::string>();
        if (author->size() > 512) return fail("'author' too long");
    }

    PluginMetadata tmp;
    tmp.schema = schema;
    tmp.id = id;
    tmp.name = name;
    tmp.version = version;
    tmp.runtime = runtime;
    tmp.language = language;
    tmp.hook_abi = hook_abi;
    tmp.targets = std::move(targets);
    tmp.entry_class = entry_class;
    tmp.description = description;
    tmp.author = author;

    std::string rl_err;
    if (!is_runtime_language_compatible(tmp, rl_err)) return fail(rl_err);

    out = std::move(tmp);
    return true;
}

bool validate_plugin_metadata_json(const std::string &json_text, PluginMetadata &out, std::string &error)
{
    if (json_text.empty()) {
        error = "metadata json is empty";
        return false;
    }
    if (json_text.size() > 64 * 1024) {
        error = "metadata json too large (max 64KiB)";
        return false;
    }
    // Reject UTF-8 BOM.
    if (json_text.size() >= 3 && (unsigned char)json_text[0] == 0xEF &&
        (unsigned char)json_text[1] == 0xBB && (unsigned char)json_text[2] == 0xBF) {
        error = "metadata json must not contain UTF-8 BOM";
        return false;
    }
    nlohmann::json j;
    try {
        j = nlohmann::json::parse(json_text, nullptr, true, true);
        if (j.is_discarded()) {
            error = "metadata json is not valid json";
            return false;
        }
    } catch (const std::exception &e) {
        error = std::string("metadata json parse error: ") + e.what();
        return false;
    }
    // The parser keeps the last duplicate key, so scan the raw text for duplicates.
    {
        const std::vector<std::string> keys = {"schema","id","name","version","runtime","language","hook_abi","targets","entry_class","description","author"};
        for (auto &k : keys) {
            std::string pat = "\"" + k + "\"";
            size_t first = json_text.find(pat);
            if (first != std::string::npos) {
                size_t second = json_text.find(pat, first + pat.size());
                if (second != std::string::npos) {
                    auto is_key = [&](size_t pos) -> bool {
                        size_t after = pos + pat.size();
                        while (after < json_text.size() && std::isspace((unsigned char)json_text[after])) ++after;
                        return after < json_text.size() && json_text[after] == ':';
                    };
                    if (is_key(first) && is_key(second)) {
                        error = "duplicate key '" + k + "'";
                        return false;
                    }
                }
            }
        }
    }

    return validate_plugin_metadata_json(j, out, error);
}

bool has_exact_build_match(const PluginMetadata &meta, const std::string &current_build_id)
{
    const PluginTarget *ignored = nullptr;
    return has_exact_build_match(meta, current_build_id, ignored);
}

bool has_exact_build_match(const PluginMetadata &meta, const std::string &current_build_id, const PluginTarget *&matched)
{
    matched = nullptr;
    if (current_build_id.empty()) return false;
    for (auto &t : meta.targets) {
        if (t.build_id == current_build_id) {
            matched = &t;
            return true;
        }
    }
    return false;
}

} // namespace Slic3r::Plugin::Package
