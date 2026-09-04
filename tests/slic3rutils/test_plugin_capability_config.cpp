#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/package/Hash.hpp>
#include <slic3r/plugin/package/InstallState.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <nlohmann/json.hpp>

#include <algorithm>
#include <fstream>
#include <string>

using namespace Slic3r;
using namespace Slic3r::Plugin::Package;
namespace fs = boost::filesystem;
using json = nlohmann::json;
using namespace Slic3r::Test;

namespace {

std::string cur_build()
{
    std::string s = Slic3r::plugin_loader::current_build_id_string();
    if (!s.empty()) return s;
    return "windows-x86_64-00000000-0000-0000-0000-000000000000-1-aaaaaaaaaaaaaaaa";
}

} // namespace

TEST_CASE("PluginManager delete removes directory and descriptor", "[PluginManager]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("delete-plugin");

    const std::string id = "deletable";
    fs::path dir = data_dir_guard.plugins_dir() / id;
    auto meta = make_minimal_plugin_json(id, "1.0.0", "jvm", "java", {cur_build()}, "com.example.Del");
    auto jar = write_jar_with_metadata(dir, id + ".jar", meta);
    std::string hash_err;
    std::string real_hash = Plugin::Package::sha256_file_hex(jar, hash_err);
    REQUIRE(!real_hash.empty());
    InstallState st{INSTALL_STATE_SCHEMA_VERSION, jar.filename().string(), real_hash, "1.0.0", true};
    std::string err;
    REQUIRE(Plugin::Package::write_install_state(dir, st, err));

    PluginManager &mgr = PluginManager::instance();
    mgr.shutdown();
    REQUIRE(mgr.initialize());

    PluginDescriptor d;
    REQUIRE(mgr.try_get_plugin_descriptor(id, d));
    std::string del_err;
    REQUIRE(mgr.delete_plugin(id, del_err));
    CHECK(del_err.empty());
    CHECK_FALSE(fs::exists(dir));
    CHECK_FALSE(mgr.try_get_plugin_descriptor(id, d));

    mgr.shutdown();
}

TEST_CASE("restart_required blocks load", "[PluginManager]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("restart-load-block");

    const std::string id = "restartblock";
    fs::path dir = data_dir_guard.plugins_dir() / id;
    fs::create_directories(dir);

    PluginDescriptor desc;
    desc.id = id;
    desc.runtime = "native";
    desc.language = "cpp";
    desc.hook_abi = 1;
    desc.targets = std::vector<Plugin::Package::PluginTarget>{{"windows", "x86_64", cur_build()}};
    desc.metadata_valid = true;
    desc.artifact_path = (dir / (id + ".dll")).string();
    desc.plugin_root = dir.string();
    desc.enabled = true;
    desc.restart_required = true;
    desc.artifact_hash = std::string(64, 'b');

    LoadedPlugin p;
    p.descriptor = desc;

    std::string load_err;
    bool ok = Slic3r::plugin_loader::load_plugin(p, load_err);
    CHECK_FALSE(ok);
    CHECK_THAT(load_err, Catch::Matchers::ContainsSubstring("requires restart"));
}

TEST_CASE("reload preserves enabled flag and picks up new artifact", "[PluginManager]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("reload-preserve");

    const std::string id = "reloadable";
    fs::path dir = data_dir_guard.plugins_dir() / id;
    auto meta1 = make_minimal_plugin_json(id, "1.0.0", "jvm", "java", {cur_build()}, "com.example.R1");
    auto jar1 = write_jar_with_metadata(dir, id + ".jar", meta1);
    std::string h1_err;
    std::string h1 = Plugin::Package::sha256_file_hex(jar1, h1_err);
    REQUIRE(!h1.empty());
    InstallState st1{INSTALL_STATE_SCHEMA_VERSION, jar1.filename().string(), h1, "1.0.0", true};
    std::string err;
    REQUIRE(Plugin::Package::write_install_state(dir, st1, err));

    PluginManager &mgr = PluginManager::instance();
    mgr.shutdown();
    REQUIRE(mgr.initialize());

    REQUIRE(mgr.set_plugin_enabled(id, false, err));
    CHECK(mgr.is_plugin_enabled(id) == false);

    auto meta2 = make_minimal_plugin_json(id, "1.1.0", "jvm", "java", {cur_build()}, "com.example.R1");
    auto jar2 = write_jar_with_metadata(dir, id + ".jar", meta2);
    std::string h2_err;
    std::string h2 = Plugin::Package::sha256_file_hex(jar2, h2_err);
    REQUIRE(!h2.empty());
    InstallState st2{INSTALL_STATE_SCHEMA_VERSION, jar2.filename().string(), h2, "1.1.0", false};
    REQUIRE(Plugin::Package::write_install_state(dir, st2, err));

    mgr.rescan_plugins();

    PluginDescriptor d;
    REQUIRE(mgr.try_get_plugin_descriptor(id, d));
    CHECK(d.version == "1.1.0");
    CHECK(d.enabled == false);

    mgr.shutdown();
}
TEST_CASE("restart_required descriptor is treated as not loadable", "[PluginManager]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("restart-not-loadable");

    const std::string id = "guarded";
    fs::path dir = data_dir_guard.plugins_dir() / id;
    fs::create_directories(dir);

    PluginDescriptor desc;
    desc.id = id;
    desc.runtime = "native";
    desc.language = "cpp";
    desc.hook_abi = 1;
    desc.targets = std::vector<Plugin::Package::PluginTarget>{{"windows", "x86_64", cur_build()}};
    desc.metadata_valid = true;
    desc.artifact_path = (dir / (id + ".dll")).string();
    desc.plugin_root = dir.string();
    desc.restart_required = true;
    desc.enabled = true;
    desc.artifact_hash = std::string(64, 'a');

    LoadedPlugin p;
    p.descriptor = desc;
    std::string load_err;
    CHECK_FALSE(Slic3r::plugin_loader::load_plugin(p, load_err));
    CHECK_THAT(load_err, Catch::Matchers::ContainsSubstring("requires restart"));
}

TEST_CASE("incompatible plugin remains visible after failed load with build mismatch error", "[PluginManager]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("incompatible-visible");

    const std::string id = "incompat2";
    fs::path dir = data_dir_guard.plugins_dir() / id;
    std::string fake_build = cur_build() + "-mismatch";
    auto meta = make_minimal_plugin_json(id, "1.0.0", "native", "cpp", {fake_build});
    auto art = write_jar_with_metadata(dir, id + ".jar", meta);
    std::string h_err;
    std::string real_hash = Plugin::Package::sha256_file_hex(art, h_err);
    REQUIRE(!real_hash.empty());
    InstallState st{INSTALL_STATE_SCHEMA_VERSION, art.filename().string(), real_hash, "1.0.0", true};
    std::string err;
    Plugin::Package::write_install_state(dir, st, err);
    PluginManager &mgr = PluginManager::instance();
    mgr.shutdown();
    REQUIRE(mgr.initialize());

    PluginDescriptor d;
    REQUIRE(mgr.try_get_plugin_descriptor(id, d));
    if (d.metadata_valid) {
        std::string load_err;
        CHECK_FALSE(mgr.load_plugin(id, load_err));
        CHECK_THAT(load_err, Catch::Matchers::ContainsSubstring("Build mismatch"));
        CHECK(mgr.try_get_plugin_descriptor(id, d));
        CHECK(d.has_error());
        auto all = mgr.get_plugin_descriptors(true);
        bool found = std::any_of(all.begin(), all.end(), [&](auto &x) { return x.id == id; });
        CHECK(found);
        auto valid_only = mgr.get_plugin_descriptors(false);
        bool found_valid = std::any_of(valid_only.begin(), valid_only.end(), [&](auto &x) { return x.id == id; });
        CHECK_FALSE(found_valid);
    } else {
        CHECK(d.has_error());
    }

    mgr.shutdown();
}
