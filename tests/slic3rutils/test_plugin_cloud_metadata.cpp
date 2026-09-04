#include <catch2/catch_all.hpp>

#include <slic3r/plugin/package/PluginMetadata.hpp>
#include <slic3r/plugin/package/PackageReader.hpp>
#include <slic3r/plugin/package/Hash.hpp>

#include "plugin_package_fixtures.hpp"
#include "plugin_test_utils.hpp"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>

#include <fstream>

using namespace Slic3r::Plugin::Package;
using Slic3r::Test::ScopedDataDir;
namespace fs = boost::filesystem;

TEST_CASE("PE resource metadata inspected without executing code", "[PackageReader][PE]")
{
    ScopedDataDir dir("pe-inspect");
    const fs::path pe = dir.dir / "test.dll";
    const std::string json = test_fixtures::valid_metadata_json("org.example.peplugin", "1.0.0", "native", "cpp", "windows-x86_64-123e4567-e89b-12d3-a456-426614174000-1-abcdef123456", "windows");

    std::string err;
    REQUIRE(test_fixtures::write_minimal_pe_with_metadata(pe, json, err));
    INFO(err);
    REQUIRE(fs::exists(pe));

    // Reader must extract metadata without LoadLibrary or executing any entry point.
    std::string out_json;
    REQUIRE(read_pe_metadata(pe, out_json, err));
    CHECK(out_json.find("org.example.peplugin") != std::string::npos);

    PluginMetadata meta;
    REQUIRE(read_pe_metadata(pe, meta, err));
    CHECK(meta.id == "org.example.peplugin");
    CHECK(meta.version == "1.0.0");
    CHECK(meta.runtime == "native");
    CHECK(meta.language == "cpp");
    REQUIRE(meta.targets.size() == 1);
    CHECK(meta.targets[0].build_id == "windows-x86_64-123e4567-e89b-12d3-a456-426614174000-1-abcdef123456");

    std::string dispatch_json;
    REQUIRE(read_plugin_metadata_file(pe, dispatch_json, err));
    CHECK(dispatch_json == out_json);

    InspectResult ir = inspect_plugin_file(pe);
    REQUIRE(ir.ok);
    CHECK(ir.metadata.id == "org.example.peplugin");
    CHECK(ir.artifact_hash.size() == 64);
    CHECK(is_hex64(ir.artifact_hash));
    // SHA computed matches direct file hash
    std::string hash_err;
    std::string direct = sha256_file_hex(pe, hash_err);
    CHECK(direct == ir.artifact_hash);
}

TEST_CASE("ELF note metadata inspected without executing code", "[PackageReader][ELF]")
{
    ScopedDataDir dir("elf-inspect");
    const fs::path so = dir.dir / "test.so";
    const std::string json = test_fixtures::valid_metadata_json("org.example.elfplugin", "0.2.0", "native", "rust", "linux-x86_64-aaaaaaaaaaaaaaaa-bbbbbbbbbbbb", "linux");

    std::string err;
    REQUIRE(test_fixtures::write_minimal_elf_with_metadata(so, json, err));
    REQUIRE(fs::exists(so));

    std::string out_json;
    REQUIRE(read_elf_metadata(so, out_json, err));
    CHECK(out_json.find("org.example.elfplugin") != std::string::npos);

    PluginMetadata meta;
    REQUIRE(read_elf_metadata(so, meta, err));
    CHECK(meta.id == "org.example.elfplugin");
    CHECK(meta.language == "rust");
    CHECK(meta.targets[0].os == "linux");

    InspectResult ir = inspect_plugin_file(so);
    REQUIRE(ir.ok);
    CHECK(ir.metadata.id == "org.example.elfplugin");
    CHECK(ir.artifact_hash.size() == 64);
}

TEST_CASE("JAR compressed metadata inspected without classloading", "[PackageReader][JAR]")
{
    ScopedDataDir dir("jar-inspect");
    const fs::path jar = dir.dir / "test.jar";
    const std::string json = test_fixtures::valid_metadata_json("org.example.javaplugin", "2.0.0", "jvm", "java", "windows-x86_64-123e4567-e89b-12d3-a456-426614174000-1-abcdef123456", "windows");

    std::string err;
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(jar, json, err));
    REQUIRE(fs::exists(jar));

    // No classloading must occur; reader uses miniz only.
    std::string out_json;
    REQUIRE(read_jar_metadata(jar, out_json, err));
    CHECK(out_json.find("org.example.javaplugin") != std::string::npos);

    PluginMetadata meta;
    REQUIRE(read_jar_metadata(jar, meta, err));
    CHECK(meta.id == "org.example.javaplugin");
    CHECK(meta.runtime == "jvm");
    REQUIRE(meta.entry_class.has_value());
    CHECK(*meta.entry_class == "com.example.TestPlugin");

    InspectResult ir = inspect_plugin_file(jar);
    REQUIRE(ir.ok);
    CHECK(ir.metadata.id == "org.example.javaplugin");
}

TEST_CASE("package reader rejects missing metadata entry without executing", "[PackageReader]")
{
    ScopedDataDir dir("missing-meta");

    // PE without resource
    {
        const fs::path pe = dir.dir / "empty.dll";
        std::vector<unsigned char> empty(512, 0);
        empty[0] = 'M'; empty[1] = 'Z';
        boost::nowide::ofstream out(pe.string(), std::ios::binary);
        out.write(reinterpret_cast<char*>(empty.data()), empty.size());
        out.close();
        std::string err;
        std::string json;
        CHECK_FALSE(read_pe_metadata(pe, json, err));
    }

    // ELF without note section
    {
        const fs::path so = dir.dir / "empty.so";
        boost::nowide::ofstream out(so.string(), std::ios::binary);
        out << "not elf";
        out.close();
        std::string err;
        std::string json;
        CHECK_FALSE(read_elf_metadata(so, json, err));
    }

    // JAR without plugin.json
    {
        const fs::path jar = dir.dir / "empty.jar";
        mz_zip_archive zip{};
        mz_zip_zero_struct(&zip);
        REQUIRE(mz_zip_writer_init_file(&zip, jar.string().c_str(), 0));
        const char *data = "hello";
        REQUIRE(mz_zip_writer_add_mem(&zip, "other.txt", data, strlen(data), 0));
        REQUIRE(mz_zip_writer_finalize_archive(&zip));
        REQUIRE(mz_zip_writer_end(&zip));
        std::string err;
        std::string json;
        CHECK_FALSE(read_jar_metadata(jar, json, err));
        CHECK_THAT(err, Catch::Matchers::ContainsSubstring("missing entry"));
    }
}

TEST_CASE("JAR path traversal entries are rejected", "[PackageReader][JAR]")
{
    ScopedDataDir dir("jar-traversal");
    const fs::path jar = dir.dir / "evil.jar";
    const std::string json = test_fixtures::valid_metadata_json();
    std::string err;
    REQUIRE(test_fixtures::write_jar_with_traversal_entry(jar, json, err));
    std::string out;
    // Reader must detect ".." in any entry and reject, even though plugin.json is present.
    CHECK_FALSE(read_jar_metadata(jar, out, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring(".."));
}

TEST_CASE("exact build match requires identical string", "[PackageReader][BuildId]")
{
    PluginMetadata meta;
    meta.targets = {
        {"windows", "x86_64", "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456"},
        {"linux", "x86_64", "linux-x86_64-1111111111112222-aaaaaaaaaaaa"}
    };

    CHECK(has_exact_build_match(meta, "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456"));
    CHECK_FALSE(has_exact_build_match(meta, "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123457")); // one char diff
    CHECK_FALSE(has_exact_build_match(meta, "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef12345")); // prefix
    CHECK_FALSE(has_exact_build_match(meta, "WINDOWS-X86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456")); // case sensitive
    CHECK_FALSE(has_exact_build_match(meta, ""));
    CHECK(has_exact_build_match(meta, "linux-x86_64-1111111111112222-aaaaaaaaaaaa"));

    const PluginTarget *matched = nullptr;
    CHECK(has_exact_build_match(meta, "linux-x86_64-1111111111112222-aaaaaaaaaaaa", matched));
    REQUIRE(matched != nullptr);
    CHECK(matched->os == "linux");

    CHECK_FALSE(has_exact_build_match(meta, "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456 ", matched)); // trailing space
}

TEST_CASE("SHA256 file hash round-trip is deterministic", "[PackageReader][Hash]")
{
    ScopedDataDir dir("hash-roundtrip");
    const fs::path f = dir.dir / "data.bin";
    {
        boost::nowide::ofstream out(f.string(), std::ios::binary);
        out << "abc123";
    }
    std::string err;
    std::string h1 = sha256_file_hex(f, err);
    REQUIRE_FALSE(h1.empty());
    CHECK(h1.size() == 64);
    CHECK(is_hex64(h1));

    // Same content => same hash
    std::string h2 = sha256_file_hex(f, err);
    CHECK(h1 == h2);

    // String helper matches file helper for same bytes
    std::string hs = sha256_string_hex("abc123");
    CHECK(hs == h1);

    // Changing one byte changes hash
    {
        boost::nowide::ofstream out(f.string(), std::ios::binary | std::ios::trunc);
        out << "abc124";
    }
    std::string h3 = sha256_file_hex(f, err);
    CHECK(h3 != h1);
}

TEST_CASE("JAR inspection validates metadata schema after decompression", "[PackageReader][JAR]")
{
    ScopedDataDir dir("jar-schema");
    const fs::path jar = dir.dir / "bad.jar";

    // Valid JAR but JSON has invalid id
    nlohmann::json j;
    j["schema"] = 1;
    j["id"] = "../evil";
    j["name"] = "Bad";
    j["version"] = "1.0.0";
    j["runtime"] = "native";
    j["language"] = "cpp";
    j["hook_abi"] = 1;
    j["targets"] = {{{"os","windows"},{"arch","x86_64"},{"build_id","windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456"}}};
    std::string bad_json = j.dump();
    std::string err;
    REQUIRE(test_fixtures::write_minimal_jar_with_metadata(jar, bad_json, err));
    PluginMetadata meta;
    CHECK_FALSE(read_jar_metadata(jar, meta, err));
    CHECK_THAT(err, Catch::Matchers::ContainsSubstring("id"));

    InspectResult ir = inspect_plugin_file(jar);
    CHECK_FALSE(ir.ok);
}

TEST_CASE("PE and ELF readers reject tampered image hash mismatch via build_id check", "[PackageReader][BuildId]")
{
    // Build mismatch is not a reader error; reader extracts metadata, but has_exact_build_match decides compatibility.
    ScopedDataDir dir("build-mismatch");

    const std::string build_a = "windows-x86_64-aaaaaaaa-bbbb-cccc-dddd-eeeeeeeeeeee-1-abcdef123456";
    const std::string build_b = "windows-x86_64-bbbbbbbb-cccc-dddd-eeee-ffffffffffff-1-123456abcdef";

    const fs::path pe = dir.dir / "a.dll";
    std::string err;
    const std::string json = test_fixtures::valid_metadata_json("org.example.mismatch", "1.0.0", "native", "cpp", build_a, "windows");
    REQUIRE(test_fixtures::write_minimal_pe_with_metadata(pe, json, err));

    PluginMetadata meta;
    REQUIRE(read_pe_metadata(pe, meta, err));
    CHECK(has_exact_build_match(meta, build_a));
    CHECK_FALSE(has_exact_build_match(meta, build_b));

    CHECK_FALSE(has_exact_build_match(meta, build_b));
}
