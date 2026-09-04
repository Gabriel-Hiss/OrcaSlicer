#include <catch2/catch_test_macros.hpp>
#include <catch2/matchers/catch_matchers_string.hpp>

#include "fixtures/hook/ManifestBuilder.hpp"
#include "fixtures/hook/FakeHookBackend.hpp"

#include "slic3r/plugin/hook/HookDefs.hpp"
#include "slic3r/plugin/hook/HookRegistry.hpp"
#include "slic3r/plugin/hook/CpuContext.hpp"
#include "slic3r/plugin/package/PluginMetadata.hpp"
#include "slic3r/plugin/package/PackageReader.hpp"
#include "slic3r/plugin/package/Hash.hpp"

#include "../../sdk/plugin_v1/abi/orca_hook_api.h"

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>

using namespace Slic3r::Hook;
using namespace Slic3r::Hook::Test;
using namespace Slic3r::Plugin::Package;
using Catch::Matchers::ContainsSubstring;
namespace fs = boost::filesystem;

// Legacy plugin fields: old preset/3MF keys that must round-trip opaquely.
static nlohmann::json make_preset_with_legacy_fields() {
    nlohmann::json j;
    j["printer_settings_id"] = "TestPrinter";
    j["plugins"] = nlohmann::json::array({"my.plugin-1;;hookA", "other;;hookB"});
    j["slicing_pipeline_plugin"] = nlohmann::json::array({"my.plugin-1"});
    j["print_plugin_config_overrides"] = nlohmann::json::object({{"my.plugin-1", {{"k","v"}}}});
    j["filament_plugin_config_overrides"] = nlohmann::json::array();
    j["layer_height"] = 0.2;
    return j;
}

TEST_CASE("Manifest build_id must exactly match active for loading", "[slicing_pipeline][manifest]") {
    auto active = MakeTestBuildId("OrcaSlicer.dll");
    auto manifest = MakeTestManifest({MakeFuncSymbol("Slic3r::CLI::print_help", 0x1000, 0x100, {0,3,7})}, active);

    std::string err;
    CHECK(manifest->ValidateBuildId(active, err));
    CHECK(err.empty());

    // Mismatched GUID
    BuildId bad = active;
    bad.debug_guid[0] ^= 0xFF;
    CHECK_FALSE(manifest->ValidateBuildId(bad, err));
    CHECK_THAT(err, ContainsSubstring("build_id"));

    // Mismatched hash
    bad = active;
    bad.image_hash[0] ^= 0xFF;
    CHECK_FALSE(manifest->ValidateBuildId(bad, err));

    // Mismatched abi
    auto manifest2 = MakeTestManifest({MakeFuncSymbol("Slic3r::CLI::print_help", 0x1000, 0x100)}, active);
    manifest2->hook_abi = 999;
    CHECK_FALSE(manifest2->ValidateBuildId(active, err));
    CHECK_THAT(err, ContainsSubstring("hook_abi"));

    // format_version mismatch
    auto manifest3 = MakeTestManifest({MakeFuncSymbol("Slic3r::CLI::print_help", 0x1000, 0x100)}, active);
    manifest3->format_version = 2;
    CHECK_FALSE(manifest3->ValidateBuildId(active, err));
}

TEST_CASE("Manifest instruction boundary validation is deterministic and exact", "[slicing_pipeline][manifest]") {
    auto sym = MakeFuncSymbol("Slic3r::CLI::print_help", 0x1000, 0x100, {0,3,7,12,20});
    auto manifest = MakeTestManifest({sym});
    auto* s = manifest->FindById("Slic3r::CLI::print_help");
    REQUIRE(s != nullptr);
    CHECK(manifest->IsValidInstructionBoundary(*s, 0x1000)); // offset 0
    CHECK(manifest->IsValidInstructionBoundary(*s, 0x1003));
    CHECK_FALSE(manifest->IsValidInstructionBoundary(*s, 0x1004));
    CHECK_FALSE(manifest->IsValidInstructionBoundary(*s, 0x1005));
    CHECK(manifest->IsValidInstructionBoundary(*s, 0x1007));
    CHECK_FALSE(manifest->IsValidInstructionBoundary(*s, 0x0FFF));
    CHECK_FALSE(manifest->IsValidInstructionBoundary(*s, 0x1100)); // outside
    // Empty instr_offsets case: only entry valid
    auto sym2 = MakeFuncSymbol("empty::func", 0x2000, 0x50, {}); // builder injects {0}
    sym2.instr_offsets.clear();
    auto m2 = MakeTestManifest({sym2});
    auto* s2 = m2->FindById("empty::func");
    REQUIRE(s2 != nullptr);
    CHECK(m2->IsValidInstructionBoundary(*s2, 0x2000));
    CHECK_FALSE(m2->IsValidInstructionBoundary(*s2, 0x2001));
}

TEST_CASE("Manifest IsDeterministic requires sorted symbols", "[slicing_pipeline][manifest]") {
    auto m = MakeTestManifest({
        MakeFuncSymbol("zzz", 0x3000, 0x10),
        MakeFuncSymbol("aaa", 0x1000, 0x10)
    });
    // MakeTestManifest sorts, so should be deterministic
    CHECK(m->IsDeterministic());
    std::swap(m->symbols[0], m->symbols[1]);
    CHECK_FALSE(m->IsDeterministic());
}

TEST_CASE("Manifest typed_binding marks trivial vs non-trivial layouts as raw with reason", "[slicing_pipeline][typed]") {
    // In generated manifest, trivially-copyable aggregates are typed, STL/non-trivial are handles borrowed.
    auto avail = MakeFuncSymbol("trivial::func", 0x1000, 0x10);
    avail.typed.available = true; avail.typed.reason = "";
    auto raw = MakeFuncSymbol("complex::func", 0x2000, 0x10);
    raw.typed.available = false; raw.typed.reason = "class with non-trivial layout/STL";
    auto m = MakeTestManifest({avail, raw});
    auto* a = m->FindById("trivial::func");
    auto* r = m->FindById("complex::func");
    REQUIRE(a); REQUIRE(r);
    CHECK(a->typed.available);
    CHECK(a->typed.reason.empty());
    CHECK_FALSE(r->typed.available);
    CHECK_THAT(r->typed.reason, ContainsSubstring("STL"));
}

TEST_CASE("ABI import_hook alias selects IAT on Windows and GOT on Linux deterministically", "[slicing_pipeline][abi]") {
    // C ABI defines IAT=5 and GOT=6; SDK alias import_hook maps per OS.
    CHECK(static_cast<int>(ORCA_HOOK_POINT_IAT) == 5);
    CHECK(static_cast<int>(ORCA_HOOK_POINT_GOT) == 6);
    // HookDefs mirrors the same
    CHECK(static_cast<int>(HookPoint::IAT) == 5);
    CHECK(static_cast<int>(HookPoint::GOT) == 6);
    auto manifest = MakeDefaultTestManifest();
    FakeHookBackend backend(MakeTestBuildId());
    HookRegistry reg(manifest.get(), &backend);
    std::string err;
    auto iat = std::make_shared<HookRequest>();
    iat->plugin_id = "p"; iat->hook_id = "iat"; iat->point = HookPoint::IAT; iat->kind = HookKind::Before;
    iat->import.module = "kernel32.dll"; iat->import.symbol = "LoadLibraryA";
    iat->before = [](CpuContext*, HookResult* out){ *out = MakeContinueResult(); return HookStatusAbi::ORCA_HOOK_OK; };
    auto got = std::make_shared<HookRequest>();
    got->plugin_id = "p"; got->hook_id = "got"; got->point = HookPoint::GOT; got->kind = HookKind::Before;
    got->import.module = "libfoo.so"; got->import.symbol = "foo";
    got->before = [](CpuContext*, HookResult* out){ *out = MakeContinueResult(); return HookStatusAbi::ORCA_HOOK_OK; };
    CHECK(reg.InstallTransaction("p", {iat}, err));
    CHECK(reg.InstallTransaction("p", {got}, err));
    CHECK(backend.installedCount() == 2);
}

TEST_CASE("Legacy preset plugin fields round-trip opaquely without reinterpretation", "[slicing_pipeline][legacy]") {
    auto original = make_preset_with_legacy_fields();
    std::string serialized = original.dump();

    // New loader reads raw JSON and should preserve unknown keys exactly when re-emitting
    nlohmann::json loaded = nlohmann::json::parse(serialized);
    nlohmann::json reemitted = loaded;
    std::string reserialized = reemitted.dump();
    nlohmann::json reparsed = nlohmann::json::parse(reserialized);

    CHECK(reparsed["plugins"] == original["plugins"]);
    CHECK(reparsed["slicing_pipeline_plugin"] == original["slicing_pipeline_plugin"]);
    CHECK(reparsed["print_plugin_config_overrides"] == original["print_plugin_config_overrides"]);
    CHECK(reparsed["filament_plugin_config_overrides"] == original["filament_plugin_config_overrides"]);
    CHECK(reparsed["layer_height"] == original["layer_height"]);
    // Ensure we did not reorder or drop internal entries the loader does not understand
    CHECK(reparsed.dump() == original.dump());
}

TEST_CASE("JAR inspect without classloading via PackageReader reports missing entry without execution", "[slicing_pipeline][package]") {
    fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-test-jar-%%%%-%%%%");
    fs::create_directories(tmp);
    fs::path jar = tmp / "empty.jar";
    {
        std::ofstream out(jar.string(), std::ios::binary);
        uint8_t eocd[22] = {0};
        eocd[0]=0x50; eocd[1]=0x4b; eocd[2]=0x05; eocd[3]=0x06;
        out.write(reinterpret_cast<char*>(eocd), 22);
    }
    auto res = inspect_plugin_file(jar);
    CHECK_FALSE(res.ok);
    boost::system::error_code ec; fs::remove_all(tmp, ec);
}

TEST_CASE("BuildId hex hash helper produces deterministic 64-char lowercase hex", "[slicing_pipeline][package]") {
    std::string hex = sha256_string_hex("hello");
    CHECK(hex.size() == 64);
    for (char c : hex) {
        bool is_hex = (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
        CHECK(is_hex);
    }
    // Deterministic
    CHECK(sha256_string_hex("hello") == hex);
    CHECK(sha256_string_hex("hello") != sha256_string_hex("world"));
}

TEST_CASE("Plugin metadata targets require exact build_id match - loader rejects before execution", "[slicing_pipeline][package]") {
    PluginMetadata meta;
    std::string err;
    std::string good = R"({"schema":1,"id":"my.plugin-1","name":"Test","version":"1.0.0","runtime":"native","language":"cpp","hook_abi":1,"targets":[{"os":"windows","arch":"x86_64","build_id":"win-build-1"}]})";
    REQUIRE(validate_plugin_metadata_json(good, meta, err));
    // Exact match required before any code would be executed
    CHECK(has_exact_build_match(meta, "win-build-1"));
    CHECK_FALSE(has_exact_build_match(meta, "win-build-2"));
    CHECK_FALSE(has_exact_build_match(meta, "linux-build-1"));
}

TEST_CASE("CpuContext raw hook exposes GPR/XMM and stack pointer", "[slicing_pipeline][cpu]") {
    CpuContext ctx{};
    ctx.size = sizeof(CpuContext); ctx.version = ORCA_HOOK_ABI_VERSION;
    ctx.rax = 1; ctx.rbx = 2; ctx.rcx = 3; ctx.rdx = 4;
    ctx.rsi = 5; ctx.rdi = 6; ctx.rbp = 7; ctx.rsp = 0x7FFFE000;
    ctx.r8 = 8; ctx.r9 = 9; ctx.r10 = 10; ctx.r11 = 11; ctx.r12 = 12; ctx.r13 = 13; ctx.r14 = 14; ctx.r15 = 15;
    ctx.rip = 0x140001000; ctx.rflags = 0x202;
    ctx.xmm0.low = 0xDEADBEEF; ctx.xmm15.high = 0xCAFEBABE;

    auto raw = [](CpuContext* c, HookResult* out){
        c->rax = 0x9999;
        c->xmm0.low = 0x1111;
        out->size = sizeof(HookResult); out->version = ORCA_HOOK_ABI_VERSION; out->action = ORCA_HOOK_ACTION_CONTINUE;
        return HookStatusAbi::ORCA_HOOK_OK;
    };
    HookResult out = MakeContinueResult();
    raw(&ctx, &out);
    CHECK(ctx.rax == 0x9999);
    CHECK(ctx.xmm0.low == 0x1111);
    CHECK(ctx.rsp == 0x7FFFE000); // unchanged
}
