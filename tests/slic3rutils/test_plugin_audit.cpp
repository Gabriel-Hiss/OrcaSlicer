#include <catch2/catch_all.hpp>

#include <slic3r/plugin/package/PluginMetadata.hpp>

#include <nlohmann/json.hpp>

#include <string>

using namespace Slic3r::Plugin::Package;

static nlohmann::json make_valid_json()
{
    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = "org.example.testplugin";
    j["name"] = "Test Plugin";
    j["version"] = "1.2.3";
    j["runtime"] = "native";
    j["language"] = "cpp";
    j["hook_abi"] = 1;
    j["targets"] = nlohmann::json::array();
    nlohmann::json t;
    t["os"] = "windows";
    t["arch"] = "x86_64";
    t["build_id"] = "windows-x86_64-123e4567-e89b-12d3-a456-426614174000-1-abcdef123456";
    j["targets"].push_back(t);
    return j;
}

TEST_CASE("plugin id validation rejects path traversal and malformed ids", "[PluginMetadata]")
{
    CHECK(is_valid_plugin_id("abc"));
    CHECK(is_valid_plugin_id("a1b2_c3.d-4"));
    CHECK(is_valid_plugin_id("org.example.plugin_v2"));
    CHECK_FALSE(is_valid_plugin_id(""));
    CHECK_FALSE(is_valid_plugin_id("ab")); // too short
    CHECK_FALSE(is_valid_plugin_id("Abc")); // uppercase
    CHECK_FALSE(is_valid_plugin_id("org/example"));
    CHECK_FALSE(is_valid_plugin_id("org\\example"));
    CHECK_FALSE(is_valid_plugin_id("../escape"));
    CHECK_FALSE(is_valid_plugin_id("a/b"));
    CHECK_FALSE(is_valid_plugin_id("a..b")); // contains ..
    CHECK_FALSE(is_valid_plugin_id(".."));
    CHECK_FALSE(is_valid_plugin_id("a b")); // space
    std::string long_id(129, 'a');
    CHECK_FALSE(is_valid_plugin_id(long_id));
    CHECK(is_valid_plugin_id(std::string(128, 'a')));
}

TEST_CASE("plugin metadata valid schema passes", "[PluginMetadata]")
{
    auto j = make_valid_json();
    PluginMetadata out;
    std::string err;
    REQUIRE(validate_plugin_metadata_json(j, out, err));
    CHECK(err.empty());
    CHECK(out.id == "org.example.testplugin");
    CHECK(out.version == "1.2.3");
    CHECK(out.runtime == "native");
    CHECK(out.language == "cpp");
    CHECK(out.targets.size() == 1);
}

TEST_CASE("plugin metadata rejects missing required fields", "[PluginMetadata]")
{
    const std::vector<std::string> required = {"schema","id","name","version","runtime","language","hook_abi","targets"};
    for (auto &field : required) {
        auto j = make_valid_json();
        j.erase(field);
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring(field));
    }
}

TEST_CASE("plugin metadata rejects unknown fields", "[PluginMetadata]")
{
    auto j = make_valid_json();
    j["unknown_field"] = 123;
    PluginMetadata out;
    std::string err;
    REQUIRE_FALSE(validate_plugin_metadata_json(j, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("unknown field"));

    auto j2 = make_valid_json();
    j2["targets"][0]["extra"] = "bad";
    REQUIRE_FALSE(validate_plugin_metadata_json(j2, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("unknown field"));
}

TEST_CASE("plugin metadata duplicate keys are rejected", "[PluginMetadata]")
{
    // Raw JSON with duplicate id key
    std::string raw = R"({"schema":1,"id":"a.b.c","id":"a.b.c","name":"Test Plugin","version":"1.0.0","runtime":"native","language":"cpp","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-111111111111"}]})";
    PluginMetadata out;
    std::string err;
    REQUIRE_FALSE(validate_plugin_metadata_json(raw, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("duplicate key"));

    std::string raw2 = R"({"schema":1,"id":"org.example.test","name":"Test Plugin","version":"1.0.0","runtime":"native","language":"cpp","hook_abi":1,"hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-111111111111"}]})";
    REQUIRE_FALSE(validate_plugin_metadata_json(raw2, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("duplicate key"));
}

TEST_CASE("plugin metadata semver validation", "[PluginMetadata]")
{
    auto check_version = [](const std::string &v, bool expect_ok) {
        auto j = make_valid_json();
        j["version"] = v;
        PluginMetadata out;
        std::string err;
        bool ok = validate_plugin_metadata_json(j, out, err);
        CHECK(ok == expect_ok);
        CHECK(is_valid_semver(v) == expect_ok);
    };
    check_version("1.0.0", true);
    check_version("0.1.0", true);
    check_version("1.2.3-alpha", true);
    check_version("1.2.3+build.1", true);
    check_version("1.2.3-alpha+001", true);
    check_version("10.20.30", true);
    check_version("", false);
    check_version("1.0", false);
    check_version("v1.0.0", false);
    check_version("1.0.0.0", false);
    check_version("01.0.0", false);
    check_version("1.0.0-", false);
}

TEST_CASE("plugin metadata runtime language compatibility", "[PluginMetadata]")
{
    auto check = [](const std::string &runtime, const std::string &language, bool expect_ok, bool with_entry = false) {
        auto j = make_valid_json();
        j["runtime"] = runtime;
        j["language"] = language;
        if (with_entry) j["entry_class"] = "com.example.Foo";
        else j.erase("entry_class");
        PluginMetadata out;
        std::string err;
        bool ok = validate_plugin_metadata_json(j, out, err);
        if (expect_ok) {
            CHECK(ok);
        } else {
            CHECK_FALSE(ok);
        }
    };
    // native requires cpp|rust, no entry_class
    check("native", "cpp", true);
    check("native", "rust", true);
    check("native", "java", false);
    check("native", "kotlin", false);
    check("native", "cpp", false, true); // entry_class must not be present

    // jvm requires java|kotlin with entry_class
    check("jvm", "java", false); // missing entry_class
    check("jvm", "kotlin", false);
    check("jvm", "java", true, true);
    check("jvm", "kotlin", true, true);
    check("jvm", "cpp", false, true);
    check("jvm", "rust", false, true);

    // entry_class slash form rejected
    {
        auto j = make_valid_json();
        j["runtime"] = "jvm";
        j["language"] = "java";
        j["entry_class"] = "com/example/Foo";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("dot-separated"));
    }
}

TEST_CASE("plugin metadata hook_abi and schema version must be 1", "[PluginMetadata]")
{
    {
        auto j = make_valid_json();
        j["hook_abi"] = 2;
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("hook_abi"));
    }
    {
        auto j = make_valid_json();
        j["schema"] = 2;
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("schema"));
    }
}

TEST_CASE("plugin metadata targets validation", "[PluginMetadata]")
{
    // empty targets
    {
        auto j = make_valid_json();
        j["targets"] = nlohmann::json::array();
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    // invalid os
    {
        auto j = make_valid_json();
        j["targets"][0]["os"] = "macos";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("os"));
    }
    // invalid arch
    {
        auto j = make_valid_json();
        j["targets"][0]["arch"] = "arm64";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    // duplicate build_id
    {
        auto j = make_valid_json();
        j["targets"].push_back(j["targets"][0]);
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("duplicate"));
    }
    // build_id with path traversal
    {
        auto j = make_valid_json();
        j["targets"][0]["build_id"] = "../escape";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    {
        auto j = make_valid_json();
        j["targets"][0]["build_id"] = "windows/x64";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    // build_id whitespace
    {
        auto j = make_valid_json();
        j["targets"][0]["build_id"] = "windows x86_64 foo";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    // too many targets
    {
        auto j = make_valid_json();
        j["targets"] = nlohmann::json::array();
        for (int i = 0; i < 17; ++i) {
            nlohmann::json t;
            t["os"] = "windows";
            t["arch"] = "x86_64";
            t["build_id"] = "build-" + std::to_string(i);
            j["targets"].push_back(t);
        }
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
}

TEST_CASE("plugin metadata path traversal in id is rejected", "[PluginMetadata]")
{
    const std::vector<std::string> bad_ids = {
        "../evil",
        "a/b",
        "a\\b",
        "a..b",
        "..",
        "org..example",
        "/absolute",
        "a/b/c"
    };
    for (auto &bad : bad_ids) {
        auto j = make_valid_json();
        j["id"] = bad;
        PluginMetadata out;
        std::string err;
        INFO("id: " << bad);
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
        CHECK_FALSE(is_valid_plugin_id(bad));
    }
}

TEST_CASE("plugin metadata entry_class required for jvm and forbidden for native", "[PluginMetadata]")
{
    // native with entry_class present
    {
        auto j = make_valid_json();
        j["runtime"] = "native";
        j["language"] = "cpp";
        j["entry_class"] = "com.example.Foo";
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
    // jvm without entry_class
    {
        auto j = make_valid_json();
        j["runtime"] = "jvm";
        j["language"] = "java";
        j.erase("entry_class");
        PluginMetadata out;
        std::string err;
        CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    }
}

TEST_CASE("safe filename and path component validation", "[PluginMetadata]")
{
    CHECK(is_safe_filename("plugin.dll"));
    CHECK(is_safe_filename("my-plugin_1.0.jar"));
    CHECK_FALSE(is_safe_filename(""));
    CHECK_FALSE(is_safe_filename("../evil.dll"));
    CHECK_FALSE(is_safe_filename("a/b.dll"));
    CHECK_FALSE(is_safe_filename("a\\b.dll"));
    CHECK_FALSE(is_safe_filename("."));
    CHECK_FALSE(is_safe_filename(".."));
    CHECK_FALSE(is_safe_filename("a:b.dll"));
    CHECK_FALSE(is_safe_filename(std::string("a\0b", 3)));

    CHECK(is_safe_path_component("abc"));
    CHECK(is_safe_path_component("org.example.plugin_v2"));
    CHECK_FALSE(is_safe_path_component("../escape"));
    CHECK_FALSE(is_safe_path_component("Abc"));
}
