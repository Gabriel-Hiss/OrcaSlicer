#include <catch2/catch_all.hpp>

#include <libslic3r/Config.hpp>
#include <libslic3r/PrintConfig.hpp>
#include <libslic3r/Preset.hpp>

#include <memory>
#include <string>
#include <vector>
#include <map>

using namespace Slic3r;

TEST_CASE("legacy plugins field round-trips through DynamicPrintConfig JSON", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    std::vector<std::string> legacy_plugins = {"myplugin;;hookA", "other;;hookB", "legacy;;entry"};
    config.set_key_value("plugins", new ConfigOptionStrings(legacy_plugins));

    std::map<std::string, std::string> serialized;
    serialized["plugins"] = config.option<ConfigOptionStrings>("plugins")->serialize();

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(serialized, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.option<ConfigOptionStrings>("plugins")->values == legacy_plugins);
}

TEST_CASE("legacy slicing_pipeline_plugin field round-trips through DynamicPrintConfig", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    std::vector<std::string> pipeline = {"hookA", "hookB"};
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings(pipeline));

    std::map<std::string, std::string> serialized{
        {"slicing_pipeline_plugin", config.option<ConfigOptionStrings>("slicing_pipeline_plugin")->serialize()}
    };

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(serialized, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == pipeline);
}

TEST_CASE("legacy plugin override fields survive string-map serialization", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    const std::string print_override = R"([{"type":"hook","id":"a","config":{"alpha":1}}])";
    const std::string printer_override = R"({"hook":"printer","value":42})";
    const std::string filament_override = R"([{"filament":"pla"}])";

    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(print_override));
    config.set_key_value("printer_plugin_config_overrides", new ConfigOptionString(printer_override));
    config.set_key_value("filament_plugin_config_overrides", new ConfigOptionString(filament_override));

    std::map<std::string, std::string> serialized{
        {"print_plugin_config_overrides", config.option<ConfigOptionString>("print_plugin_config_overrides")->serialize()},
        {"printer_plugin_config_overrides", config.option<ConfigOptionString>("printer_plugin_config_overrides")->serialize()},
        {"filament_plugin_config_overrides", config.option<ConfigOptionString>("filament_plugin_config_overrides")->serialize()},
    };

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(serialized, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.option<ConfigOptionString>("print_plugin_config_overrides")->value == print_override);
    CHECK(reloaded.option<ConfigOptionString>("printer_plugin_config_overrides")->value == printer_override);
    CHECK(reloaded.option<ConfigOptionString>("filament_plugin_config_overrides")->value == filament_override);
}

TEST_CASE("all five legacy fields together round-trip without loss or reordering", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("plugins", new ConfigOptionStrings({"p1;;h1", "p2;;h2"}));
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({"h1"}));
    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(R"({"k":"v"})"));
    config.set_key_value("printer_plugin_config_overrides", new ConfigOptionString(R"([1,2,3])"));
    config.set_key_value("filament_plugin_config_overrides", new ConfigOptionString(R"("opaque")"));

    std::map<std::string, std::string> smap;
    for (auto key : {"plugins", "slicing_pipeline_plugin", "print_plugin_config_overrides",
                     "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        if (auto opt = config.option(key)) {
            smap[key] = opt->serialize();
        }
    }

    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);

    CHECK(reloaded.option<ConfigOptionStrings>("plugins")->values == std::vector<std::string>{"p1;;h1", "p2;;h2"});
    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values == std::vector<std::string>{"h1"});
    CHECK(reloaded.option<ConfigOptionString>("print_plugin_config_overrides")->value == R"({"k":"v"})");
    CHECK(reloaded.option<ConfigOptionString>("printer_plugin_config_overrides")->value == R"([1,2,3])");
    CHECK(reloaded.option<ConfigOptionString>("filament_plugin_config_overrides")->value == R"("opaque")");
}

TEST_CASE("legacy fields preserve exact json values including empty and special characters", "[Config][Legacy]")
{
    DynamicPrintConfig config = DynamicPrintConfig::full_print_config();
    config.set_key_value("plugins", new ConfigOptionStrings({}));
    config.set_key_value("slicing_pipeline_plugin", new ConfigOptionStrings({}));
    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(""));
    config.set_key_value("printer_plugin_config_overrides", new ConfigOptionString(""));
    config.set_key_value("filament_plugin_config_overrides", new ConfigOptionString(""));

    std::map<std::string, std::string> smap;
    for (auto key : {"plugins", "slicing_pipeline_plugin", "print_plugin_config_overrides",
                     "printer_plugin_config_overrides", "filament_plugin_config_overrides"}) {
        smap[key] = config.option(key)->serialize();
    }
    DynamicPrintConfig reloaded = DynamicPrintConfig::full_print_config();
    reloaded.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(reloaded.option<ConfigOptionStrings>("plugins")->values.empty());
    CHECK(reloaded.option<ConfigOptionStrings>("slicing_pipeline_plugin")->values.empty());
    CHECK(reloaded.option<ConfigOptionString>("print_plugin_config_overrides")->value.empty());

    const std::string tricky = R"({"a":"b\"c\nd","emoji":"\u263A"})";
    config.set_key_value("print_plugin_config_overrides", new ConfigOptionString(tricky));
    smap["print_plugin_config_overrides"] = config.option<ConfigOptionString>("print_plugin_config_overrides")->serialize();
    DynamicPrintConfig reloaded2 = DynamicPrintConfig::full_print_config();
    reloaded2.load_string_map(smap, ForwardCompatibilitySubstitutionRule::Disable);
    CHECK(reloaded2.option<ConfigOptionString>("print_plugin_config_overrides")->value == tricky);
}
