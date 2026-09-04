#pragma once

#include "PluginStatus.hpp"
#include "PluginSort.hpp"

#include <slic3r/plugin/PluginDescriptor.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>

#include <nlohmann/json.hpp>

#include <string>
#include <vector>

namespace Slic3r::GUI {

struct PluginContextAction
{
    std::string id;
    std::string label;
    bool enabled = true;
    bool danger  = false;
};

struct PluginAvailableActions
{
    bool can_toggle = false;
    std::vector<PluginContextAction> context_actions;
};

struct PluginDialogItem
{
    std::string id;
    std::string display_name;
    std::string name;
    std::string description;
    std::string author;
    std::string version;
    std::string language;
    std::string runtime;
    std::vector<Slic3r::Plugin::Package::PluginTarget> targets;
    std::string current_build_id;
    bool compatible = false;
    std::string artifact_path;
    std::string artifact_hash;
    PluginStatus status = PluginStatus::Inactive;
    std::string error_text;
    bool has_error = false;
    bool enabled = false;
    bool loaded = false;
    bool restart_required = false;
    bool can_toggle = false;
    std::string sort_version;
    std::string type_key;
    PluginAvailableActions available_actions;
};

std::string get_current_build_id_fallback();

nlohmann::json build_context_actions_payload(const PluginAvailableActions& aa);
nlohmann::json build_plugin_payload_item(const PluginDialogItem& it);
PluginAvailableActions evaluate_action_policy(const PluginDialogItem& it);

// Primary: queries PluginManager for loaded state (uses current process state).
PluginDialogItem descriptor_to_item(const PluginDescriptor& d, const std::string& cur_build);

// Headless/testable overload: caller supplies loaded flag and optional load error explicitly.
PluginDialogItem descriptor_to_item(const PluginDescriptor& d,
                                    const std::string& cur_build,
                                    bool is_loaded,
                                    const std::string& load_error);

} // namespace Slic3r::GUI
