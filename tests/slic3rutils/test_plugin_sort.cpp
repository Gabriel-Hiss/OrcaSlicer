#include <catch2/catch_all.hpp>

#include <slic3r/GUI/PluginSort.hpp>

#include <string>
#include <vector>

using Slic3r::GUI::compare_ascii_case_insensitive_natural;
using Slic3r::GUI::PluginSortKey;
using Slic3r::GUI::PluginSortOrder;
using Slic3r::GUI::PluginStatus;
using Slic3r::GUI::plugin_sort_key_from_string;
using Slic3r::GUI::plugin_sort_order_from_string;
using Slic3r::GUI::sort_plugin_items_for_dialog;

namespace {

struct SortFixtureItem
{
    std::string id;
    PluginStatus status = PluginStatus::Inactive;
    std::string display_name;
    std::string version;
};

std::vector<std::string> keys(const std::vector<SortFixtureItem>& items)
{
    std::vector<std::string> result;
    result.reserve(items.size());
    for (const SortFixtureItem& item : items)
        result.push_back(item.id);
    return result;
}

} // namespace

TEST_CASE("plugin dialog status sort uses requested priority and base-order ties", "[PluginSort]")
{
    std::vector<SortFixtureItem> items = {
        {"inactive",     PluginStatus::Inactive,       "Inactive Plugin",  "1.0.0"},
        {"error",        PluginStatus::Error,          "Error Plugin",     "1.0.0"},
        {"activated_b",  PluginStatus::Activated,      "B Activated",      "1.0.0"},
        {"activated_a",  PluginStatus::Activated,      "A Activated",      "1.0.0"},
        {"incompatible", PluginStatus::Incompatible,   "Incompatible Plugin","1.0.0"},
        {"restart",      PluginStatus::RestartRequired,"Restart Plugin",   "1.0.0"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Status, PluginSortOrder::Asc);

    const std::vector<std::string> expected = {
        "activated_a",
        "activated_b",
        "error",
        "incompatible",
        "inactive",
        "restart",
    };
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Status, PluginSortOrder::Desc);

    const std::vector<std::string> desc_expected = {
        "restart",
        "inactive",
        "incompatible",
        "error",
        "activated_a",
        "activated_b",
    };
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("plugin dialog version sort is semver-aware with base-order ties", "[PluginSort]")
{
    std::vector<SortFixtureItem> items = {
        {"v_1_2_0",  PluginStatus::Activated, "B", "1.2.0"},
        {"v_1_10_0", PluginStatus::Activated, "A", "1.10.0"},
        {"v_0_9_3",  PluginStatus::Activated, "C", "0.9.3"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Version, PluginSortOrder::Asc);
    const std::vector<std::string> asc_expected = {"v_0_9_3", "v_1_2_0", "v_1_10_0"};
    CHECK(keys(items) == asc_expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Version, PluginSortOrder::Desc);
    const std::vector<std::string> desc_expected = {"v_1_10_0", "v_1_2_0", "v_0_9_3"};
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("plugin dialog name sort is case-insensitive and numeric-aware", "[PluginSort]")
{
    std::vector<SortFixtureItem> items = {
        {"rig10", PluginStatus::Activated, "Rig 10", "1.0.0"},
        {"ada_lower", PluginStatus::Activated, "ada", "1.0.0"},
        {"rig2", PluginStatus::Activated, "Rig 2", "1.0.0"},
        {"ada_upper", PluginStatus::Activated, "Ada", "1.0.0"},
    };

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Asc);
    const std::vector<std::string> expected = {"ada_lower", "ada_upper", "rig2", "rig10"};
    CHECK(keys(items) == expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Desc);
    const std::vector<std::string> desc_expected = {"rig10", "rig2", "ada_lower", "ada_upper"};
    CHECK(keys(items) == desc_expected);
}

TEST_CASE("natural compare handles digits, case, prefixes and leading zeros", "[PluginSort]")
{
    CHECK(compare_ascii_case_insensitive_natural("item2", "item10") < 0);
    CHECK(compare_ascii_case_insensitive_natural("item10", "item2") > 0);
    CHECK(compare_ascii_case_insensitive_natural("2", "10") < 0);

    CHECK(compare_ascii_case_insensitive_natural("Camera", "camera") == 0);

    CHECK(compare_ascii_case_insensitive_natural("app", "apple") < 0);
    CHECK(compare_ascii_case_insensitive_natural("apple", "app") > 0);

    CHECK(compare_ascii_case_insensitive_natural("1", "01") < 0);
    CHECK(compare_ascii_case_insensitive_natural("01", "1") > 0);

    CHECK(compare_ascii_case_insensitive_natural("plugin", "plugin") == 0);
    CHECK(compare_ascii_case_insensitive_natural("", "") == 0);
    CHECK(compare_ascii_case_insensitive_natural("", "a") < 0);
}

TEST_CASE("plugin dialog None sort key falls to ascending base order in both directions", "[PluginSort]")
{
    std::vector<SortFixtureItem> items = {
        {"z_plugin", PluginStatus::Activated, "Zebra", "1.0.0"},
        {"a_plugin", PluginStatus::Activated, "Apple", "1.0.0"},
        {"m_plugin", PluginStatus::Error,     "Mango", "1.0.0"},
    };

    const std::vector<std::string> base_expected = {"a_plugin", "m_plugin", "z_plugin"};

    sort_plugin_items_for_dialog(items, PluginSortKey::None, PluginSortOrder::Asc);
    CHECK(keys(items) == base_expected);

    sort_plugin_items_for_dialog(items, PluginSortKey::None, PluginSortOrder::Desc);
    CHECK(keys(items) == base_expected);
}

TEST_CASE("plugin dialog sort request parsing keeps previous state on invalid values", "[PluginSort]")
{
    CHECK(plugin_sort_key_from_string("name", PluginSortKey::Status) == PluginSortKey::Name);
    CHECK(plugin_sort_key_from_string("none", PluginSortKey::Status) == PluginSortKey::None);
    CHECK(plugin_sort_key_from_string("missing", PluginSortKey::Name) == PluginSortKey::Name);

    CHECK(plugin_sort_order_from_string("desc", PluginSortOrder::Asc) == PluginSortOrder::Desc);
    CHECK(plugin_sort_order_from_string("down", PluginSortOrder::Asc) == PluginSortOrder::Asc);
}

TEST_CASE("plugin descriptor fields used for sorting are stable and deterministic", "[PluginSort]")
{
    std::vector<SortFixtureItem> items = {
        {"b_key", PluginStatus::Activated, "Same Name", "1.0.0"},
        {"a_key", PluginStatus::Activated, "Same Name", "1.0.0"},
    };
    sort_plugin_items_for_dialog(items, PluginSortKey::Name, PluginSortOrder::Asc);
    CHECK(keys(items) == std::vector<std::string>{"a_key", "b_key"});

    CHECK(compare_ascii_case_insensitive_natural("1.0.0-alpha", "1.0.0-beta") < 0);
}
