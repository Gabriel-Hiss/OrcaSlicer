#include <catch2/catch_all.hpp>

#include <slic3r/plugin/package/InstallState.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>
#include <slic3r/plugin/package/PackageReader.hpp>
#include <slic3r/plugin/package/Hash.hpp>
#include <slic3r/plugin/PluginLoader.hpp>

#include <nlohmann/json.hpp>

#include "plugin_package_fixtures.hpp"
#include "plugin_test_utils.hpp"

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <fstream>
#include <set>
#include <string>
using namespace Slic3r::Plugin::Package;
using Slic3r::Test::ScopedDataDir;
using Slic3r::data_dir;
namespace fs = boost::filesystem;

TEST_CASE("install state round-trip preserves artifact hash version and enabled", "[PluginInstall][InstallState]")
{
    ScopedDataDir dir("install-state-roundtrip");
    const fs::path plugin_dir = dir.dir / "org.example.testplugin";
    fs::create_directories(plugin_dir);

    InstallState st;
    st.schema = 1;
    st.artifact = "testplugin.dll";
    st.hash = "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890";
    st.version = "1.2.3";
    st.enabled = true;

    std::string err;
    REQUIRE(write_install_state(plugin_dir, st, err));

    InstallState out;
    REQUIRE(read_install_state(plugin_dir, out, err));
    CHECK(out.schema == 1);
    CHECK(out.artifact == "testplugin.dll");
    CHECK(out.hash == "abcdef1234567890abcdef1234567890abcdef1234567890abcdef1234567890");
    CHECK(out.version == "1.2.3");
    CHECK(out.enabled == true);

    st.enabled = false;
    st.version = "2.0.0-alpha+001";
    st.hash = "AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA";
    // writer normalizes hash to lowercase
    REQUIRE(write_install_state(plugin_dir, st, err));
    REQUIRE(read_install_state(plugin_dir, out, err));
    CHECK(out.enabled == false);
    CHECK(out.version == "2.0.0-alpha+001");
    CHECK(out.hash == "aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa");
}

TEST_CASE("install state rejects invalid artifact and hash", "[PluginInstall][InstallState]")
{
    ScopedDataDir dir("install-state-invalid");
    const fs::path file = dir.dir / "state" / ".install_state.json";
    fs::create_directories(file.parent_path());

    auto try_write = [&](InstallState s) {
        std::string err;
        bool ok = write_install_state_file(file, s, err);
        return std::make_pair(ok, err);
    };

    InstallState base;
    base.schema = 1;
    base.artifact = "plugin.dll";
    base.hash = std::string(64, 'a');
    base.version = "1.0.0";
    base.enabled = true;

    // invalid artifact with path traversal
    {
        InstallState s = base;
        s.artifact = "../evil.dll";
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("artifact"));
    }
    // invalid extension
    {
        InstallState s = base;
        s.artifact = "plugin.txt";
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
    }
    // invalid hash length
    {
        InstallState s = base;
        s.hash = "abc123";
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
    }
    // invalid hash hex
    {
        InstallState s = base;
        s.hash = std::string(64, 'z');
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
    }
    // invalid version
    {
        InstallState s = base;
        s.version = "not-semver";
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
    }
    // invalid schema
    {
        InstallState s = base;
        s.schema = 2;
        auto [ok, err] = try_write(s);
        CHECK_FALSE(ok);
    }
}

TEST_CASE("install state duplicate keys are rejected on read", "[PluginInstall][InstallState]")
{
    ScopedDataDir dir("install-state-duplicate");
    const fs::path file = dir.dir / ".install_state.json";
    // Write JSON with duplicate "artifact" key manually
    std::string raw = R"({"schema":1,"artifact":"a.dll","artifact":"b.dll","hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":"1.0.0","enabled":true})";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary);
        out << raw;
    }
    InstallState out;
    std::string err;
    CHECK_FALSE(read_install_state_file(file, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("duplicate"));
}

TEST_CASE("install state unknown fields are rejected", "[PluginInstall][InstallState]")
{
    ScopedDataDir dir("install-state-unknown");
    const fs::path file = dir.dir / ".install_state.json";
    std::string raw = R"({"schema":1,"artifact":"a.dll","hash":"aaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaaa","version":"1.0.0","enabled":true,"extra":123})";
    {
        boost::nowide::ofstream out(file.string(), std::ios::binary);
        out << raw;
    }
    InstallState out;
    std::string err;
    CHECK_FALSE(read_install_state_file(file, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("unknown field"));
}

TEST_CASE("SHA file hash matches string hash and is deterministic", "[PluginInstall][Hash]")
{
    ScopedDataDir dir("sha-determinism");
    const fs::path f = dir.dir / "payload.bin";
    const std::string content = "Deterministic content for hashing 12345";
    {
        boost::nowide::ofstream out(f.string(), std::ios::binary);
        out << content;
    }
    std::string err;
    std::string file_hash = sha256_file_hex(f, err);
    REQUIRE_FALSE(file_hash.empty());
    CHECK(file_hash.size() == 64);
    CHECK(is_hex64(file_hash));
    std::string str_hash = sha256_string_hex(content);
    CHECK(file_hash == str_hash);

    // Second read same
    std::string file_hash2 = sha256_file_hex(f, err);
    CHECK(file_hash == file_hash2);

    // Tamper changes hash
    {
        boost::nowide::ofstream out(f.string(), std::ios::binary | std::ios::trunc);
        out << content << "x";
    }
    std::string file_hash3 = sha256_file_hex(f, err);
    CHECK(file_hash3 != file_hash);
}

TEST_CASE("inspect computes SHA and validates metadata without executing entry", "[PluginInstall][Inspect]")
{
    ScopedDataDir dir("inspect-no-exec");
    const fs::path jar = dir.dir / "plugin.jar";
    const std::string json = test_fixtures::valid_metadata_json("org.example.noexec", "1.0.0", "jvm", "kotlin", "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456", "windows");
    std::string err;
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(jar, json, err));

    // Inspect must read metadata and hash without loading the JAR or running entry_class.
    InspectResult ir = inspect_plugin_file(jar);
    REQUIRE(ir.ok);
    CHECK(ir.metadata.id == "org.example.noexec");
    CHECK(ir.metadata.entry_class.has_value());
    CHECK(ir.artifact_hash.size() == 64);
    std::string direct = sha256_file_hex(jar, err);
    CHECK(ir.artifact_hash == direct);
    CHECK(ir.json_text.find("org.example.noexec") != std::string::npos);
}

TEST_CASE("transactional overwrite preserves existing artifact and state on failure", "[PluginInstall][Transactional]")
{
    ScopedDataDir dir("transactional-preserve");
    const fs::path plugins_root = fs::path(data_dir()) / "orca_plugins";
    fs::create_directories(plugins_root);

    const std::string plugin_id = "org.example.preserve";
    const fs::path src_valid = dir.dir / "src_valid.jar";
    const std::string json_valid = test_fixtures::valid_metadata_json(plugin_id, "1.0.0", "jvm", "java", "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456", "windows");
    std::string err;
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(src_valid, json_valid, err));

    Slic3r::PluginDescriptor desc;
    bool ok = Slic3r::plugin_loader::install_plugin(src_valid, desc, err);
    INFO(err);
    REQUIRE(ok);
    CHECK(desc.id == plugin_id);
    const fs::path installed_dir = plugins_root / plugin_id;
    REQUIRE(fs::exists(installed_dir));
    const fs::path installed_state = installed_dir / ".install_state.json";
    REQUIRE(fs::exists(installed_state));
    std::string orig_hash = desc.artifact_hash;
    REQUIRE(orig_hash.size() == 64);
    const fs::path installed_artifact = fs::path(desc.artifact_path);
    REQUIRE(fs::exists(installed_artifact));
    std::string orig_artifact_content;
    {
        boost::nowide::ifstream in(installed_artifact.string(), std::ios::binary);
        orig_artifact_content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    InstallState orig_state;
    REQUIRE(read_install_state(installed_dir, orig_state, err));
    CHECK(orig_state.version == "1.0.0");

    // Now attempt to overwrite with an invalid package (missing plugin.json)
    const fs::path src_invalid = dir.dir / "src_invalid.jar";
    {
        mz_zip_archive zip{};
        mz_zip_zero_struct(&zip);
        REQUIRE(mz_zip_writer_init_file(&zip, src_invalid.string().c_str(), 0));
        const char *data = "not a plugin";
        REQUIRE(mz_zip_writer_add_mem(&zip, "other.txt", data, strlen(data), 0));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(mz_zip_writer_end(&zip));
    }

    Slic3r::PluginDescriptor desc2;
    bool ok2 = Slic3r::plugin_loader::install_plugin(src_invalid, desc2, err);
    CHECK_FALSE(ok2);
    // Existing installation must be untouched
    CHECK(fs::exists(installed_artifact));
    CHECK(fs::exists(installed_state));
    InstallState after_state;
    REQUIRE(read_install_state(installed_dir, after_state, err));
    CHECK(after_state.hash == orig_state.hash);
    CHECK(after_state.version == orig_state.version);
    CHECK(after_state.artifact == orig_state.artifact);
    std::string after_content;
    {
        boost::nowide::ifstream in(installed_artifact.string(), std::ios::binary);
        after_content.assign(std::istreambuf_iterator<char>(in), std::istreambuf_iterator<char>());
    }
    CHECK(after_content == orig_artifact_content);

    // Attempt with invalid metadata (path traversal id) also preserves
    const fs::path src_bad_meta = dir.dir / "src_bad.jar";
    nlohmann::json j = nlohmann::json::parse(json_valid);
    j["id"] = "../evil";
    std::string bad_json = j.dump();
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(src_bad_meta, bad_json, err));
    bool ok3 = Slic3r::plugin_loader::install_plugin(src_bad_meta, desc2, err);
    CHECK_FALSE(ok3);
    REQUIRE(read_install_state(installed_dir, after_state, err));
    CHECK(after_state.hash == orig_state.hash);

    // Successful overwrite with newer version should update state atomically
    const fs::path src_new = dir.dir / "src_new.jar";
    const std::string json_new = test_fixtures::valid_metadata_json(plugin_id, "1.1.0", "jvm", "java", "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456", "windows");
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(src_new, json_new, err));
    bool ok4 = Slic3r::plugin_loader::install_plugin(src_new, desc2, err);
    INFO(err);
    REQUIRE(ok4);
    InstallState new_state;
    REQUIRE(read_install_state(installed_dir, new_state, err));
    CHECK(new_state.version == "1.1.0");
    CHECK(new_state.hash != orig_state.hash);
    CHECK(new_state.hash.size() == 64);
}

TEST_CASE("allowed artifact extensions are enforced case-insensitively", "[PluginInstall]")
{
    std::string ext;
    CHECK(is_allowed_artifact_extension(fs::path("plugin.dll"), ext));
    CHECK(ext == ".dll");
    CHECK(is_allowed_artifact_extension(fs::path("plugin.DLL"), ext));
    CHECK(ext == ".dll");
    CHECK(is_allowed_artifact_extension(fs::path("plugin.so"), ext));
    CHECK(is_allowed_artifact_extension(fs::path("plugin.jar"), ext));
    CHECK_FALSE(is_allowed_artifact_extension(fs::path("plugin.txt"), ext));
    CHECK_FALSE(is_allowed_artifact_extension(fs::path("plugin.py"), ext));

    CHECK(is_safe_plugin_id_for_fs("org.example.foo"));
    CHECK_FALSE(is_safe_plugin_id_for_fs("../evil"));
    CHECK_FALSE(is_safe_plugin_id_for_fs("Abc"));
}

TEST_CASE("plugin id path traversal rejected before filesystem copy", "[PluginInstall]")
{
    ScopedDataDir dir("id-traversal");
    const fs::path src = dir.dir / "evil.jar";
    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = "../../escape";
    j["name"] = "Evil";
    j["version"] = "1.0.0";
    j["runtime"] = "native";
    j["language"] = "cpp";
    j["hook_abi"] = 1;
    j["targets"] = {{{"os","windows"},{"arch","x86_64"},{"build_id","windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456"}}};
    std::string bad_json = j.dump();
    std::string err;
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(src, bad_json, err));

    fs::path data_path = fs::path(data_dir());
    fs::path plugins_root = data_path / "orca_plugins";
    std::set<std::string> before_data;
    if (fs::exists(data_path)) {
        for (fs::directory_iterator it(data_path), end; it != end; ++it) before_data.insert(it->path().filename().string());
    }
    std::set<std::string> before_plugins;
    if (fs::exists(plugins_root)) {
        for (fs::directory_iterator it(plugins_root), end; it != end; ++it) before_plugins.insert(it->path().filename().string());
    }

    Slic3r::PluginDescriptor desc;
    bool ok = Slic3r::plugin_loader::install_plugin(src, desc, err);
    CHECK_FALSE(ok);
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("id"));
    // Verify no directory or file was created outside orca_plugins and no traversal entry inside
    CHECK_FALSE(fs::exists(data_path / "escape"));
    CHECK_FALSE(fs::exists(plugins_root / "escape"));
    // Parent traversal must not have created sibling escape
    CHECK_FALSE(fs::exists(data_path.parent_path() / "escape"));
    if (fs::exists(data_path)) {
        for (fs::directory_iterator it(data_path), end; it != end; ++it) {
            CHECK(before_data.count(it->path().filename().string()) == 1);
        }
    }
    if (fs::exists(plugins_root)) {
        for (fs::directory_iterator it(plugins_root), end; it != end; ++it) {
            CHECK(before_plugins.count(it->path().filename().string()) == 1);
        }
    }
}
