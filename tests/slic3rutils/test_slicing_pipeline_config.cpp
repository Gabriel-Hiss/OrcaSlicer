#include <catch2/catch_all.hpp>

#include <libslic3r/Config.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>
#include <libslic3r/PresetBundle.hpp>

#include <boost/filesystem.hpp>
#include <boost/nowide/fstream.hpp>
#include <nlohmann/json.hpp>

#include <fstream>
#include <string>
#include <map>

using namespace Slic3r;
namespace fs = boost::filesystem;
using json = nlohmann::json;

TEST_CASE("Preset and PrintConfig keep legacy fields as inert opaque data", "[Preset][Legacy]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    for (auto key : {"plugins", "slicing_pipeline_plugin", "print_plugin_config_overrides",
                     "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        INFO("checking key: " << key);
        CHECK(cfg.def()->get(key) != nullptr);
    }
    CHECK(cfg.option<ConfigOptionString>("print_plugin_config_overrides") != nullptr);
    CHECK(cfg.option<ConfigOptionString>("printer_plugin_config_overrides") != nullptr);
    CHECK(cfg.option<ConfigOptionString>("filament_plugin_config_overrides") != nullptr);
    CHECK(cfg.option<ConfigOptionStrings>("plugins") != nullptr);
    CHECK(cfg.option<ConfigOptionStrings>("slicing_pipeline_plugin") != nullptr);
}

TEST_CASE("legacy pipeline and overrides survive Preset config save and reload", "[Preset][Legacy]")
{
    Preset preset(Preset::TYPE_PRINT, "test-legacy-preset");
    preset.config = DynamicPrintConfig::full_print_config();

    preset.config.set_key_value("plugins", new ConfigOptionStrings({"plugA;;hook1", "plugB;;hook2"}));
    preset.config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"hook1"}));
    preset.config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(R"({"alpha":1,"beta":"x"})"));
    preset.config.set_key_value("printer_plugin_config_overrides", new ConfigOptionString(R"([])"));
    preset.config.set_key_value("filament_plugin_config_overrides", new ConfigOptionString(R"("keep")"));

    std::map<std::string, std::string> smap;
    for (auto key : {"plugins", "slicing_pipeline_plugin", "print_plugin_config_overrides",
                     "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        smap[key] = preset.config.option(key)->serialize();
    }

    Preset reloaded(Preset::TYPE_PRINT, "test-legacy-preset");
    reloaded.config = DynamicPrintConfig::full_print_config();
    reloaded.config.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.config.option<ConfigOptionStrings>("plugins")->values == std::vector<std::string>{"plugA;;hook1", "plugB;;hook2"});
    CHECK(reloaded.config.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == std::vector<std::string>{"hook1"});
    CHECK(reloaded.config.option<ConfigOptionString>("print_plugin_config_overrides")->value == R"({"alpha":1,"beta":"x"})");
    CHECK(reloaded.config.option<ConfigOptionString>("printer_plugin_config_overrides")->value == R"([])");
    CHECK(reloaded.config.option<ConfigOptionString>("filament_plugin_config_overrides")->value == R"("keep")");
}

TEST_CASE("legacy override keys are literal strings and live on correct preset types", "[Preset][Legacy]")
{
    const std::string k_print    = "print_plugin_config_overrides";
    const std::string k_printer  = "printer_plugin_config_overrides";
    const std::string k_filament = "filament_plugin_config_overrides";
    CHECK(k_print == std::string("print_plugin_config_overrides"));
    CHECK(k_printer == std::string("printer_plugin_config_overrides"));
    CHECK(k_filament == std::string("filament_plugin_config_overrides"));

    const std::pair<Preset::Type, const std::vector<std::string>*> scopes[] = {
        {Preset::TYPE_PRINT,    &Preset::print_options()},
        {Preset::TYPE_PRINTER,  &Preset::printer_options()},
        {Preset::TYPE_FILAMENT, &Preset::filament_options()},
    };
    auto contains = [](const std::vector<std::string>& v, const std::string& s){
        return std::find(v.begin(), v.end(), s) != v.end();
    };
    for (auto &scoped : scopes) {
        const std::string key = (scoped.first == Preset::TYPE_PRINT ? k_print :
                                 scoped.first == Preset::TYPE_PRINTER ? k_printer : k_filament);
        for (auto &owner : scopes) {
            CAPTURE(key);
            CAPTURE(owner.first);
            CHECK(contains(*owner.second, key) == (owner.first == scoped.first));
        }
    }
}

TEST_CASE("DynamicPrintConfig save_to_json preserves legacy fields exactly", "[Config][Legacy]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    cfg.set_key_value("plugins", new ConfigOptionStrings({"a;;h1", "b;;h2"}));
    cfg.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"h1", "h2"}));
    cfg.set_key_value("print_plugin_config_overrides", new ConfigOptionString(R"({"nested":{"x":1}})" ));
    cfg.set_key_value("printer_plugin_config_overrides", new ConfigOptionString(R"("raw")"));
    cfg.set_key_value("filament_plugin_config_overrides", new ConfigOptionString(R"([{"f":"pla"}])"));

    fs::path tmp = fs::temp_directory_path() / fs::unique_path("orca-legacy-%%%%-%%%%.json");
    cfg.save_to_json(tmp.string(), "test_preset", "User", "9.9.9");
    boost::nowide::ifstream ifs(tmp.string());
    REQUIRE(ifs.good());
    json j; ifs >> j;
    ifs.close();
    fs::remove(tmp);

    CHECK(j.contains("plugins"));
    CHECK(j.contains("slicing_pipeline_plugin"));
    CHECK(j.contains("print_plugin_config_overrides"));
    CHECK(j.contains("printer_plugin_config_overrides"));
    CHECK(j.contains("filament_plugin_config_overrides"));

    CHECK(j["plugins"] == json::array({"a;;h1", "b;;h2"}));
    CHECK(j["slicing_pipeline_plugin"] == json::array({"h1", "h2"}));
    CHECK(j["print_plugin_config_overrides"] == std::string(R"({"nested":{"x":1}})"));
    CHECK(j["printer_plugin_config_overrides"] == std::string(R"("raw")"));
    CHECK(j["filament_plugin_config_overrides"] == std::string(R"([{"f":"pla"}])"));

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    std::map<std::string, std::string> smap;
    for (auto key : {"plugins", "slicing_pipeline_plugin", "print_plugin_config_overrides",
                     "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        if (j.contains(key)) {
            if (j[key].is_array()) {
                std::vector<std::string> vals = j[key].get<std::vector<std::string>>();
                ConfigOptionStrings opt(vals);
                smap[key] = opt.serialize();
            } else if (j[key].is_string()) {
                ConfigOptionString opt(j[key].get<std::string>());
                smap[key] = opt.serialize();
            }
        }
    }
    reloaded.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(reloaded.option<ConfigOptionStrings>("plugins")->values == std::vector<std::string>{"a;;h1", "b;;h2"});
    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == std::vector<std::string>{"h1", "h2"});
    CHECK(reloaded.option<ConfigOptionString>("print_plugin_config_overrides")->value == R"({"nested":{"x":1}})");
}

TEST_CASE("legacy fields missing from file remain empty, not error", "[Config][Legacy]")
{
    DynamicPrintConfig cfg = DynamicPrintConfig::full_print_config();
    CHECK(cfg.option<ConfigOptionStrings>("plugins")->values.empty());
    CHECK(cfg.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values.empty());
    CHECK(cfg.option<ConfigOptionString>("print_plugin_config_overrides")->value.empty());

    std::map<std::string, std::string> smap;
    smap["plugins"] = cfg.option<ConfigOptionStrings>("plugins")->serialize();
    smap["slicing_pipeline_plugin"] = cfg.option<ConfigOptionStrings>("slicing_pipeline_plugin")->serialize();
    smap["print_plugin_config_overrides"] = cfg.option<ConfigOptionString>("print_plugin_config_overrides")->serialize();

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(reloaded.option<ConfigOptionStrings>("plugins")->values.empty());
    CHECK(reloaded.option<ConfigOptionString>("print_plugin_config_overrides")->value.empty());
}
