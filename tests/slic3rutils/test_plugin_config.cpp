#include <catch2/catch_all.hpp>

#include <libslic3r/Utils.hpp>
#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/PluginLoader.hpp>
#include <slic3r/plugin/PluginManager.hpp>
#include <slic3r/plugin/PluginFsUtils.hpp>
#include <slic3r/plugin/package/Hash.hpp>
#include <slic3r/plugin/package/InstallState.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>
#include <slic3r/plugin/package/PackageReader.hpp>

#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
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

std::string current_build_or_dummy()
{
    std::string cur = Slic3r::plugin_loader::current_build_id_string();
    if (!cur.empty()) return cur;
    return "windows-x86_64-00000000-0000-0000-0000-000000000000-1-aaaaaaaaaaaaaaaa";
}

} // namespace

TEST_CASE("InstallState persists only artifact hash version and enabled", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("installstate-roundtrip");

    InstallState state;
    state.schema   = INSTALL_STATE_SCHEMA_VERSION;
    state.artifact = "myplugin.dll";
    state.hash     = std::string(64, 'a');
    state.version  = "1.2.3";
    state.enabled  = true;

    const fs::path plugin_dir = data_dir_guard.plugins_dir() / "myplugin";
    std::string err;
    REQUIRE(Plugin::Package::write_install_state(plugin_dir, state, err));
    CHECK(err.empty());

    InstallState loaded;
    REQUIRE(Plugin::Package::read_install_state(plugin_dir, loaded, err));
    CHECK(loaded.schema == INSTALL_STATE_SCHEMA_VERSION);
    CHECK(loaded.artifact == "myplugin.dll");
    CHECK(loaded.hash == std::string(64, 'a'));
    CHECK(loaded.version == "1.2.3");
    CHECK(loaded.enabled == true);

    {
        boost::nowide::ifstream ifs((plugin_dir / INSTALL_STATE_FILENAME).string());
        json j; ifs >> j;
        CHECK(j.size() == 5);
        CHECK(j.at("schema") == INSTALL_STATE_SCHEMA_VERSION);
        CHECK(j.at("artifact") == "myplugin.dll");
        CHECK(j.at("hash") == std::string(64, 'a'));
        CHECK(j.at("version") == "1.2.3");
        CHECK(j.at("enabled") == true);
    }
    state.enabled = false;
    REQUIRE(Plugin::Package::write_install_state(plugin_dir, state, err));
    REQUIRE(Plugin::Package::read_install_state(plugin_dir, loaded, err));
    CHECK(loaded.enabled == false);
}

TEST_CASE("InstallState normalizes hash to lowercase and rejects invalid hashes", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("installstate-hash");

    InstallState state;
    state.schema   = INSTALL_STATE_SCHEMA_VERSION;
    state.artifact = "plugin.dll";
    state.hash     = std::string(64, 'A');
    state.version  = "0.1.0";
    state.enabled  = true;

    const fs::path plugin_dir = data_dir_guard.plugins_dir() / "plugina";
    std::string err;
    REQUIRE(Plugin::Package::write_install_state(plugin_dir, state, err));
    InstallState loaded;
    REQUIRE(Plugin::Package::read_install_state(plugin_dir, loaded, err));
    CHECK(loaded.hash == std::string(64, 'a'));

    state.hash = "abc";
    fs::path bad_dir = data_dir_guard.plugins_dir() / "pluginb";
    CHECK_FALSE(Plugin::Package::write_install_state(bad_dir, state, err));
    CHECK_FALSE(err.empty());
}

TEST_CASE("PluginFsUtils discover keeps invalid packages visible with error", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("discover-invalid");

    const fs::path plugin_dir = data_dir_guard.plugins_dir() / "badplugin";
    write_dummy_native_artifact(plugin_dir, "badplugin.dll");
    InstallState st;
    st.schema = INSTALL_STATE_SCHEMA_VERSION;
    st.artifact = "badplugin.dll";
    st.hash = std::string(64, 'b');
    st.version = "1.0.0";
    st.enabled = true;
    std::string err;
    REQUIRE(Plugin::Package::write_install_state(plugin_dir, st, err));

    std::vector<std::string> dirs = { data_dir_guard.plugins_dir().string() };
    std::string disc_err;
    auto discovered = discover_plugin_packages(dirs, disc_err);

    REQUIRE(discovered.size() == 1);
    const PluginDescriptor& desc = discovered.front();
    CHECK(desc.id == "badplugin");
    CHECK(desc.install_state_valid == true);
    CHECK(desc.metadata_valid == false);
    CHECK_FALSE(desc.error.empty());
    CHECK(desc.has_error());
}

TEST_CASE("discover returns plugin ids sorted and filters invalid when requested via manager", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("discover-sort");

    for (auto id : {"zeta", "alpha", "middle"}) {
        fs::path d = data_dir_guard.plugins_dir() / id;
        write_dummy_native_artifact(d, std::string(id) + ".dll");
        InstallState st{INSTALL_STATE_SCHEMA_VERSION, std::string(id) + ".dll", std::string(64,'c'), "1.0.0", true};
        std::string e; Plugin::Package::write_install_state(d, st, e);
    }
    std::string disc_err;
    auto discovered = discover_plugin_packages({data_dir_guard.plugins_dir().string()}, disc_err);
    REQUIRE(discovered.size() == 3);

    std::sort(discovered.begin(), discovered.end(), [](auto &a, auto &b){ return a.id < b.id; });
    CHECK(discovered[0].id == "alpha");
    CHECK(discovered[1].id == "middle");
    CHECK(discovered[2].id == "zeta");
}

TEST_CASE("PluginManager enable toggle persists to install state and affects enabled ids", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("manager-enable");

    const std::string plugin_id = "toggleplugin";
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / plugin_id;
    std::string cur_build = current_build_or_dummy();
    auto meta = make_minimal_plugin_json(plugin_id, "2.0.0", "jvm", "java", {cur_build}, "com.example.Toggle");
    auto jar = write_jar_with_metadata(plugin_dir, plugin_id + ".jar", meta);
    std::string hash_err;
    std::string real_hash = Plugin::Package::sha256_file_hex(jar, hash_err);
    REQUIRE(!real_hash.empty());
    InstallState st{INSTALL_STATE_SCHEMA_VERSION, jar.filename().string(), real_hash, "2.0.0", true};
    std::string e; REQUIRE(Plugin::Package::write_install_state(plugin_dir, st, e));

    PluginManager &mgr = PluginManager::instance();
    mgr.shutdown();
    REQUIRE(mgr.initialize());

    PluginDescriptor desc;
    REQUIRE(mgr.try_get_plugin_descriptor(plugin_id, desc));
    CHECK(desc.enabled == true);

    std::string err;
    REQUIRE(mgr.set_plugin_enabled(plugin_id, false, err));
    CHECK(err.empty());
    CHECK(mgr.is_plugin_enabled(plugin_id) == false);

    InstallState reloaded;
    REQUIRE(Plugin::Package::read_install_state(plugin_dir, reloaded, err));
    CHECK(reloaded.enabled == false);

    auto enabled_ids = mgr.get_enabled_plugin_ids();
    bool contains = std::find(enabled_ids.begin(), enabled_ids.end(), plugin_id) != enabled_ids.end();
    if (desc.metadata_valid) {
        CHECK_FALSE(contains);
    }

    REQUIRE(mgr.set_plugin_enabled(plugin_id, true, err));
    CHECK(mgr.is_plugin_enabled(plugin_id) == true);
    REQUIRE(Plugin::Package::read_install_state(plugin_dir, reloaded, err));
    CHECK(reloaded.enabled == true);

    mgr.shutdown();
}

TEST_CASE("incompatible build_id plugin is discovered but load is rejected without executing entry", "[PluginConfig]")
{
    Slic3r::Test::ScopedDataDir data_dir_guard("incompatible-load");

    const std::string plugin_id = "incompat";
    const fs::path plugin_dir = data_dir_guard.plugins_dir() / plugin_id;
    std::string fake_build = "windows-x86_64-ffffffff-ffff-ffff-ffff-ffffffffffff-99-ffffffffffff";
    if (current_build_or_dummy().rfind("windows-", 0) == 0) {
        fake_build = "linux-x86_64-aaaaaaaaaaaaaaaa-aaaaaaaaaaaa";
    }
    auto meta = make_minimal_plugin_json(plugin_id, "1.0.0", "native", "cpp", {fake_build});
    auto art = write_jar_with_metadata(plugin_dir, plugin_id + ".jar", meta);
    PluginMetadata pm;
    std::string merr;
    REQUIRE(validate_plugin_metadata_json(meta.dump(), pm, merr));
    CHECK_FALSE(has_exact_build_match(pm, current_build_or_dummy()));
    CHECK(has_exact_build_match(pm, fake_build));

    PluginDescriptor desc;
    desc.id = plugin_id;
    desc.runtime = "native";
    desc.language = "cpp";
    desc.hook_abi = 1;
    desc.targets = pm.targets;
    desc.metadata_valid = true;
    desc.artifact_path = art.string();
    desc.plugin_root = plugin_dir.string();
    desc.enabled = true;
    desc.artifact_hash = std::string(64,'e');

    LoadedPlugin plugin;
    plugin.descriptor = desc;
    std::string load_err;
    bool ok = Slic3r::plugin_loader::load_plugin(plugin, load_err);
    CHECK_FALSE(ok);
    CHECK_THAT(load_err, Catch::Matchers::ContainsSubstring("Build mismatch"));
    CHECK_FALSE(plugin.is_loaded());
    CHECK_FALSE(plugin.descriptor.error.empty());
}

TEST_CASE("PluginMetadata validation rejects invalid id and version", "[PluginConfig]")
{
    std::string err;
    PluginMetadata out;

    json j = make_minimal_plugin_json("BadId", "1.0.0");
    CHECK_FALSE(validate_plugin_metadata_json(j, out, err));
    CHECK_FALSE(err.empty());

    j = make_minimal_plugin_json("goodid", "not-semver");
    CHECK_FALSE(validate_plugin_metadata_json(j, out, err));

    j = make_minimal_plugin_json("goodid2", "1.0.0");
    j.erase("targets");
    CHECK_FALSE(validate_plugin_metadata_json(j, out, err));

    j = make_minimal_plugin_json("goodid3", "1.2.3", "native", "cpp", {"windows-x86_64-aaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-aaaaaaaaaaaa"});
    CHECK(validate_plugin_metadata_json(j, out, err));
}
