#pragma once

#include <libslic3r/Utils.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <cstring>
#include <string>
#include <vector>

#include "test_utils.hpp"

#if __has_include(<miniz.h>)
#include <miniz.h>
#endif
#if __has_include(<miniz/miniz.h>)
#include <miniz/miniz.h>
#endif

namespace Slic3r {
namespace Test {

// Point data_dir() at a throwaway directory for the lifetime of a test and
// restore the previous value afterwards, so code under test writes into a
// disposable tree and tests don't leak state into each other.
struct ScopedDataDir
{
    ScopedTemporaryDir        tmp;   // owns the temp dir (create + recursive remove)
    boost::filesystem::path   dir;   // = tmp.path(); kept as a member for callers
    std::string               previous;

    explicit ScopedDataDir(const std::string& tag)
        : tmp("orca-" + tag), dir(tmp.path()), previous(data_dir())
    {
        set_data_dir(dir.string());
    }

    ~ScopedDataDir() { set_data_dir(previous); } // tmp removes the directory

    // The plugin manager scans {data_dir}/orca_plugins.
    boost::filesystem::path plugins_dir() const { return dir / "orca_plugins"; }

    ScopedDataDir(const ScopedDataDir&)            = delete;
    ScopedDataDir& operator=(const ScopedDataDir&) = delete;
};

// Point resources_dir() at a throwaway directory for the lifetime of a test and restore the
// previous value afterwards, mirroring ScopedDataDir.
struct ScopedResourcesDir
{
    ScopedTemporaryDir      tmp;
    boost::filesystem::path dir;
    std::string             previous;

    explicit ScopedResourcesDir(const std::string& tag)
        : tmp("orca-" + tag), dir(tmp.path()), previous(resources_dir())
    {
        set_resources_dir(dir.string());
    }

    ~ScopedResourcesDir() { set_resources_dir(previous); }

    ScopedResourcesDir(const ScopedResourcesDir&)            = delete;
    ScopedResourcesDir& operator=(const ScopedResourcesDir&) = delete;
};


inline nlohmann::json make_minimal_plugin_json(const std::string& id,
                                               const std::string& version = "1.0.0",
                                               const std::string& runtime = "native",
                                               const std::string& language = "cpp",
                                               const std::vector<std::string>& build_ids = {"test-build-id-123"},
                                               const std::string& entry_class = "")
{
    nlohmann::json j;
    j["schema"]   = 1;
    j["id"]       = id;
    j["name"]     = id + " name";
    j["version"]  = version;
    j["runtime"]  = runtime;
    j["language"] = language;
    j["hook_abi"] = 1;
    j["targets"]  = nlohmann::json::array();
    for (auto &bid : build_ids) {
        nlohmann::json t;
        if (bid.rfind("linux-", 0) == 0) { t["os"] = "linux"; t["arch"] = "x86_64"; }
        else if (bid.rfind("windows-", 0) == 0) { t["os"] = "windows"; t["arch"] = "x86_64"; }
        else { t["os"] = "windows"; t["arch"] = "x86_64"; }
        t["build_id"] = bid;
        j["targets"].push_back(t);
    }
    if (runtime == "jvm") {
        j["entry_class"] = entry_class.empty() ? std::string("com.example.Plugin") : entry_class;
    }
    return j;
}

inline boost::filesystem::path write_jar_with_metadata(const boost::filesystem::path& dir,
                                                       const std::string& filename,
                                                       const nlohmann::json& metadata)
{
    boost::filesystem::create_directories(dir);
    boost::filesystem::path jar = dir / filename;
    std::string json_text = metadata.dump();

#if __has_include(<miniz.h>) || __has_include(<miniz/miniz.h>)
    mz_zip_archive zip{};
    std::memset(&zip, 0, sizeof(zip));
    if (mz_zip_writer_init_file(&zip, jar.string().c_str(), 0)) {
        mz_zip_writer_add_mem(&zip, "META-INF/orca/plugin.json", json_text.data(), json_text.size(), MZ_DEFAULT_COMPRESSION);
        mz_zip_writer_finalize_archive(&zip);
        mz_zip_writer_end(&zip);
        return jar;
    }
    mz_zip_writer_end(&zip);
#endif
    // Fallback: write json directly (will be read as invalid zip, useful for negative cases).
    boost::nowide::ofstream out(jar.string(), std::ios::binary);
    out << json_text;
    return jar;
}

inline boost::filesystem::path write_dummy_native_artifact(const boost::filesystem::path& dir,
                                                           const std::string& filename)
{
    boost::filesystem::create_directories(dir);
    boost::filesystem::path p = dir / filename;
    boost::nowide::ofstream out(p.string(), std::ios::binary);
    out << "dummy native artifact";
    return p;
}

} // namespace Test
} // namespace Slic3r
