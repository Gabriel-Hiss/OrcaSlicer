#include <catch2/catch_all.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>
#include <slic3r/plugin/package/PackageReader.hpp>
#include <slic3r/plugin/package/InstallState.hpp>
#include <slic3r/plugin/package/Hash.hpp>

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Test;
using Catch::Matchers::ContainsSubstring;
namespace fs = boost::filesystem;
namespace Pkg = Slic3r::Plugin::Package;

using Pkg::PluginMetadata;
using Pkg::InstallState;
using Pkg::is_valid_semver;
using Pkg::validate_plugin_metadata_json;
using Pkg::has_exact_build_match;
using Pkg::inspect_plugin_file;
using Pkg::sha256_string_hex;

// Writes dummy artifact file (not a real PE/ELF) - inspect will fail without execution
static fs::path write_dummy_artifact(const fs::path& dir, const std::string& id, const std::string& ext = ".dll") {
    fs::create_directories(dir / id);
    fs::path art = dir / id / (id + ext);
    std::ofstream out(art.string(), std::ios::binary);
    out << "dummy artifact content for " << id;
    return art;
}

static std::string valid_metadata_json(const std::string& id = "my.plugin-1",
                                       const std::string& runtime = "native",
                                       const std::string& lang = "cpp",
                                       const std::string& version = "1.2.3") {
    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = id;
    j["name"] = "Test Plugin";
    j["version"] = version;
    j["runtime"] = runtime;
    j["language"] = lang;
    j["hook_abi"] = 1;
    j["targets"] = nlohmann::json::array({{{"os","windows"},{"arch","x86_64"},{"build_id","windows-x86_64-abc-1-deadbeef"}}});
    if (runtime == "jvm") j["entry_class"] = "com.example.Plugin";
    return j.dump();
}

TEST_CASE("Plugin id validation rejects path components and enforces pattern", "[PluginLifecycle][native]") {
    CHECK(Pkg::is_valid_plugin_id("my.plugin-1"));
    CHECK(Pkg::is_valid_plugin_id("a12"));
    CHECK(Pkg::is_valid_plugin_id("ab_c.d-e"));
    CHECK_FALSE(Pkg::is_valid_plugin_id(""));
    CHECK_FALSE(Pkg::is_valid_plugin_id("ab"));
    CHECK_FALSE(Pkg::is_valid_plugin_id("Abc"));
    CHECK_FALSE(Pkg::is_valid_plugin_id("my/plugin"));
    CHECK_FALSE(Pkg::is_valid_plugin_id("../escape"));
    CHECK_FALSE(Pkg::is_valid_plugin_id(".hidden"));
    CHECK_FALSE(Pkg::is_valid_plugin_id("has space"));
    CHECK_FALSE(Pkg::is_valid_plugin_id(std::string(129, 'a')));
    CHECK(Pkg::is_valid_plugin_id(std::string(128, 'a')));
}

TEST_CASE("Semver validation normalizes and rejects invalid", "[PluginLifecycle][native]") {
    std::string norm;
    CHECK(is_valid_semver("1.2.3", &norm));
    CHECK(is_valid_semver("1.0.0-alpha+001", nullptr));
    CHECK_FALSE(is_valid_semver("not-semver", nullptr));
    CHECK_FALSE(is_valid_semver("1", nullptr));
    CHECK_FALSE(is_valid_semver("", nullptr));
}

TEST_CASE("Plugin metadata JSON schema 1 validation", "[PluginLifecycle][native]") {
    PluginMetadata out;
    std::string err;
    std::string good = valid_metadata_json();
    CHECK(validate_plugin_metadata_json(good, out, err));
    CHECK(out.id == "my.plugin-1");
    CHECK(out.runtime == "native");
    CHECK(out.language == "cpp");
    CHECK(out.hook_abi == 1);
    REQUIRE(out.targets.size() == 1);
    CHECK(out.targets[0].os == "windows");

    nlohmann::json bad = nlohmann::json::parse(good);
    bad.erase("id");
    CHECK_FALSE(validate_plugin_metadata_json(bad.dump(), out, err));
    CHECK_THAT(err, ContainsSubstring("id"));

    std::string bad_runtime = valid_metadata_json("my.plugin-1", "bogus", "cpp");
    CHECK_FALSE(validate_plugin_metadata_json(bad_runtime, out, err));

    std::string jvm_no_entry = valid_metadata_json("my.plugin-1", "jvm", "java");
    {
        auto j = nlohmann::json::parse(jvm_no_entry);
        j.erase("entry_class");
        CHECK_FALSE(validate_plugin_metadata_json(j.dump(), out, err));
    }
    std::string mismatch = valid_metadata_json("my.plugin-1", "native", "java");
    CHECK_FALSE(validate_plugin_metadata_json(mismatch, out, err));

    {
        auto j = nlohmann::json::parse(good);
        j["targets"][0]["os"] = "macos";
        CHECK_FALSE(validate_plugin_metadata_json(j.dump(), out, err));
    }
}

TEST_CASE("has_exact_build_match requires exact equality", "[PluginLifecycle][native]") {
    PluginMetadata meta;
    std::string err;
    REQUIRE(validate_plugin_metadata_json(valid_metadata_json(), meta, err));
    CHECK(has_exact_build_match(meta, "windows-x86_64-abc-1-deadbeef"));
    CHECK_FALSE(has_exact_build_match(meta, "windows-x86_64-abc-1-000000"));
    CHECK_FALSE(has_exact_build_match(meta, "linux-x86_64-abc-1-deadbeef"));
}

TEST_CASE("inspect_plugin_file does not execute artifact and reports metadata errors", "[PluginLifecycle][native]") {
    ScopedDataDir data_dir("lifecycle-native-inspect");
    auto plugin_dir = data_dir.plugins_dir() / "my.plugin-1";
    fs::create_directories(plugin_dir);
    fs::path dummy = plugin_dir / "my.plugin-1.dll";
    { std::ofstream out(dummy.string(), std::ios::binary); out << "not a pe"; }

    auto res = inspect_plugin_file(dummy);
    CHECK_FALSE(res.ok);
    CHECK_FALSE(res.error.empty());

    auto res2 = inspect_plugin_file(plugin_dir / "missing.dll");
    CHECK_FALSE(res2.ok);

    PluginMetadata m; std::string e;
    CHECK(validate_plugin_metadata_json(valid_metadata_json(), m, e));
}

TEST_CASE("InstallState round-trips through .install_state.json", "[PluginLifecycle][native]") {
    ScopedDataDir data_dir("lifecycle-installstate");
    auto plugin_dir = data_dir.plugins_dir() / "my.plugin-1";
    fs::create_directories(plugin_dir);
    fs::path art = plugin_dir / "my.plugin-1.dll";
    { std::ofstream out(art.string(), std::ios::binary); out << "artifact bytes"; }

    InstallState st;
    st.schema = Pkg::INSTALL_STATE_SCHEMA_VERSION;
    st.artifact = "my.plugin-1.dll";
    st.hash = std::string(64, 'a');
    st.version = "1.2.3";
    st.enabled = true;
    std::string err;
    REQUIRE(Pkg::write_install_state(plugin_dir, st, err));

    InstallState out;
    REQUIRE(Pkg::read_install_state(plugin_dir, out, err));
    CHECK(out.schema == 1);
    CHECK(out.artifact == "my.plugin-1.dll");
    CHECK(out.hash == st.hash);
    CHECK(out.version == "1.2.3");
    CHECK(out.enabled == true);

    st.enabled = false;
    REQUIRE(Pkg::write_install_state(plugin_dir, st, err));
    REQUIRE(Pkg::read_install_state(plugin_dir, out, err));
    CHECK(out.enabled == false);

    boost::nowide::ifstream ifs((plugin_dir / ".install_state.json").string());
    nlohmann::json j; ifs >> j;
    CHECK(j.contains("schema"));
    CHECK(j.contains("artifact"));
    CHECK(j.contains("hash"));
    CHECK(j.contains("version"));
    CHECK(j.contains("enabled"));
}

TEST_CASE("PluginFsUtils discover_plugin_packages scans data_dir and reports enabled from sidecar", "[PluginLifecycle][native]") {
    ScopedDataDir data_dir("lifecycle-discover");
    auto orca_plugins = data_dir.plugins_dir();
    std::string err;

    write_dummy_artifact(orca_plugins, "bare.plugin");

    auto enabled_dir = orca_plugins / "enabled.plugin";
    write_dummy_artifact(orca_plugins, "enabled.plugin");
    {
        InstallState st; st.artifact = "enabled.plugin.dll"; st.hash = std::string(64,'a'); st.version = "1.0.0"; st.enabled = true;
        std::string e; Pkg::write_install_state(enabled_dir, st, e);
    }
    auto disabled_dir = orca_plugins / "disabled.plugin";
    write_dummy_artifact(orca_plugins, "disabled.plugin");
    {
        InstallState st; st.artifact = "disabled.plugin.dll"; st.hash = std::string(64,'b'); st.version = "1.0.0"; st.enabled = false;
        std::string e; Pkg::write_install_state(disabled_dir, st, e);
    }
    fs::create_directories(orca_plugins / "BadId");
    { std::ofstream out((orca_plugins / "BadId" / "bad.dll").string()); out << "x"; }

    auto discovered = discover_plugin_packages({orca_plugins.string()}, err);
    auto has = [&](const std::string& id){ for (auto& d: discovered) if (d.id == id) return true; return false; };
    CHECK(has("bare.plugin"));
    CHECK(has("enabled.plugin"));
    CHECK(has("disabled.plugin"));
    CHECK_FALSE(has("BadId"));

    for (auto& d : discovered) {
        if (d.id == "bare.plugin") CHECK_FALSE(d.enabled);
        if (d.id == "enabled.plugin") CHECK(d.enabled);
        if (d.id == "disabled.plugin") CHECK_FALSE(d.enabled);
    }
    for (auto& d : discovered) if (d.id == "bare.plugin") CHECK_FALSE(d.metadata_valid);
}

TEST_CASE("is_allowed_artifact_extension allows only .dll/.so/.jar and is case-insensitive", "[PluginLifecycle][native]") {
    std::string norm;
    CHECK(Pkg::is_allowed_artifact_extension(fs::path("foo.dll"), norm)); CHECK(norm == ".dll");
    CHECK(Pkg::is_allowed_artifact_extension(fs::path("foo.DLL"), norm));
    CHECK(Pkg::is_allowed_artifact_extension(fs::path("foo.so"), norm));
    CHECK(Pkg::is_allowed_artifact_extension(fs::path("foo.JAR"), norm));
    CHECK_FALSE(Pkg::is_allowed_artifact_extension(fs::path("foo.py"), norm));
    CHECK_FALSE(Pkg::is_allowed_artifact_extension(fs::path("foo.txt"), norm));
}

TEST_CASE("delete_plugin_root refuses path traversal and mismatched id", "[PluginLifecycle][native]") {
    ScopedDataDir data_dir("lifecycle-delete");
    auto plugin_dir = data_dir.plugins_dir() / "my.plugin-1";
    fs::create_directories(plugin_dir);
    std::string err;
    CHECK_FALSE(delete_plugin_root(plugin_dir, "other.id", err));
    CHECK_THAT(err, ContainsSubstring("mismatch"));
    CHECK_FALSE(delete_plugin_root(plugin_dir, "../escape", err));
    CHECK(delete_plugin_root(plugin_dir, "my.plugin-1", err));
    CHECK_FALSE(fs::exists(plugin_dir));
}

TEST_CASE("PluginDescriptor install_state_valid tracks sidecar presence", "[PluginLifecycle][native]") {
    ScopedDataDir data_dir("lifecycle-descriptor");
    auto plugin_dir = data_dir.plugins_dir() / "my.plugin-1";
    fs::create_directories(plugin_dir);
    write_dummy_artifact(data_dir.plugins_dir(), "my.plugin-1");
    std::string err;
    auto first = discover_plugin_packages({data_dir.plugins_dir().string()}, err);
    bool found = false;
    for (auto& d: first) if (d.id == "my.plugin-1") { found = true; CHECK_FALSE(d.install_state_valid); }
    CHECK(found);
    InstallState st; st.artifact = "my.plugin-1.dll"; st.hash = std::string(64,'c'); st.version = "1.0.0"; st.enabled = true;
    std::string e; Pkg::write_install_state(plugin_dir, st, e);
    auto second = discover_plugin_packages({data_dir.plugins_dir().string()}, err);
    for (auto& d: second) if (d.id == "my.plugin-1") { CHECK(d.install_state_valid); CHECK(d.enabled); }
}

TEST_CASE("has_exact_build_match distinguishes multiple targets", "[PluginLifecycle][native]") {
    PluginMetadata meta;
    std::string err;
    nlohmann::json j = nlohmann::json::parse(valid_metadata_json());
    j["targets"] = nlohmann::json::array({
        {{"os","windows"},{"arch","x86_64"},{"build_id","win-id-1"}},
        {{"os","linux"},{"arch","x86_64"},{"build_id","linux-id-1"}}
    });
    CHECK(validate_plugin_metadata_json(j.dump(), meta, err));
    CHECK(has_exact_build_match(meta, "win-id-1"));
    CHECK(has_exact_build_match(meta, "linux-id-1"));
    CHECK_FALSE(has_exact_build_match(meta, "win-id-2"));
}
