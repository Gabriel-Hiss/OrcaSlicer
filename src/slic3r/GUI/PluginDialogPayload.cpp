#include "PluginDialogPayload.hpp"

#include <slic3r/plugin/PluginManager.hpp>

namespace Slic3r::GUI {

std::string get_current_build_id_fallback()
{
#ifdef _WIN32
    return "windows-x86_64-unknown";
#else
    return "linux-x86_64-unknown";
#endif
}

nlohmann::json build_context_actions_payload(const PluginAvailableActions& aa)
{
    nlohmann::json arr = nlohmann::json::array();
    for (auto &a : aa.context_actions) {
        nlohmann::json j;
        j["id"] = a.id;
        j["label"] = a.label;
        j["enabled"] = a.enabled;
        j["danger"] = a.danger;
        arr.push_back(std::move(j));
    }
    return arr;
}

nlohmann::json build_plugin_payload_item(const PluginDialogItem& it)
{
    nlohmann::json o;
    o["id"]            = it.id;
    o["name"]          = it.name;
    o["label"]         = it.display_name;
    o["display_name"]  = it.display_name;
    o["description"]   = it.description;
    o["author"]        = it.author;
    o["version"]       = it.version;
    o["language"]      = it.language;
    o["runtime"]       = it.runtime;
    o["status"]        = to_string(it.status);
    o["error"]         = it.error_text;
    o["has_error"]     = it.has_error;
    o["restart_required"] = it.restart_required;
    o["enabled"]       = it.enabled;
    o["loaded"]        = it.loaded;
    o["compatible"]    = it.compatible;
    o["current_build_id"] = it.current_build_id;
    o["artifact_path"] = it.artifact_path;
    o["artifact_hash"] = it.artifact_hash;
    o["can_toggle"]    = it.can_toggle;
    o["context_actions"] = build_context_actions_payload(it.available_actions);

    nlohmann::json targets = nlohmann::json::array();
    for (auto &t : it.targets) {
        nlohmann::json tj;
        tj["os"] = t.os;
        tj["arch"] = t.arch;
        tj["build_id"] = t.build_id;
        targets.push_back(std::move(tj));
    }
    o["targets"] = std::move(targets);
    return o;
}

PluginAvailableActions evaluate_action_policy(const PluginDialogItem& it)
{
    PluginAvailableActions aa;
    aa.can_toggle = it.compatible && !it.restart_required && !it.has_error;
    if (it.enabled && it.status == PluginStatus::Activated) {
        aa.can_toggle = !it.restart_required;
    }
    if (!it.enabled && it.compatible && !it.restart_required && it.status != PluginStatus::Loading) {
        aa.can_toggle = true;
    }
    if (!it.compatible) aa.can_toggle = false;
    if (it.restart_required) aa.can_toggle = false;

    auto add = [&](const char* id, const char* label, bool enabled, bool danger=false){
        aa.context_actions.push_back(PluginContextAction{id,label,enabled,danger});
    };
    add("delete_plugin", "Delete", true, true);
    add("open_folder", "Show in folder", true, false);
    add("reload_plugin", "Reload", it.compatible && !it.restart_required, false);
    return aa;
}

PluginDialogItem descriptor_to_item(const PluginDescriptor& d,
                                    const std::string& cur_build,
                                    bool is_loaded,
                                    const std::string& load_error)
{
    PluginDialogItem it;
    it.id = d.id;
    it.name = d.name;
    it.display_name = d.name.empty() ? it.id : d.name;
    it.version = d.version;
    it.sort_version = d.version;
    it.author = d.author;
    it.description = d.description;
    it.language = d.language;
    it.runtime = d.runtime;
    it.targets = d.targets;
    it.artifact_path = d.artifact_path;
    it.artifact_hash = d.artifact_hash;
    it.current_build_id = cur_build;
    it.enabled = d.enabled;
    it.restart_required = d.restart_required;
    it.type_key = d.runtime + "/" + d.language;

    bool has_match = false;
    if (!d.targets.empty() && !cur_build.empty() && cur_build.find("unknown")==std::string::npos) {
        Slic3r::Plugin::Package::PluginMetadata tmp;
        tmp.targets = d.targets;
        const Slic3r::Plugin::Package::PluginTarget* m=nullptr;
        has_match = Slic3r::Plugin::Package::has_exact_build_match(tmp, cur_build, m);
    }
    it.compatible = has_match && d.metadata_valid;

    if (!d.metadata_valid) {
        it.status = PluginStatus::Error;
        it.has_error = true;
        it.error_text = d.error.empty() ? "Invalid plugin metadata." : d.normalized_error();
    } else if (!d.error.empty()) {
        it.status = PluginStatus::Error;
        it.has_error = true;
        it.error_text = d.normalized_error();
    } else if (d.restart_required) {
        it.status = PluginStatus::RestartRequired;
        it.has_error = true;
        it.error_text = "Restart required to complete unload. Plugin remains loaded until restart.";
    } else if (!it.compatible) {
        it.status = PluginStatus::Incompatible;
        it.has_error = true;
        std::string tlist;
        for (size_t i=0;i<d.targets.size();++i){ if(i) tlist+=", "; tlist+=d.targets[i].build_id; }
        if (tlist.empty()) tlist="(no targets)";
        it.error_text = "Incompatible build. Plugin targets: " + tlist + ". Current build: " + cur_build + ".";
    } else if (d.enabled) {
        if (is_loaded) {
            it.status = PluginStatus::Activated;
        } else {
            if (!load_error.empty()) {
                it.status = PluginStatus::Error;
                it.has_error = true;
                it.error_text = load_error;
            } else {
                it.status = PluginStatus::Activated;
            }
        }
    } else {
        it.status = PluginStatus::Inactive;
        it.has_error = false;
    }

    it.has_error = (it.status==PluginStatus::Error || it.status==PluginStatus::Incompatible || it.status==PluginStatus::RestartRequired);
    if (!it.has_error) it.error_text.clear();

    // loaded reflects actual runtime loaded state when plugin is valid and active
    it.loaded = is_loaded && it.compatible && !it.has_error && it.status == PluginStatus::Activated;

    it.can_toggle = it.compatible && !it.restart_required && it.status!=PluginStatus::Loading;
    if (it.status==PluginStatus::Incompatible || it.status==PluginStatus::RestartRequired || it.status==PluginStatus::Error)
        it.can_toggle = false;
    if (it.status==PluginStatus::Inactive && it.compatible) it.can_toggle = true;
    if (it.status==PluginStatus::Activated) it.can_toggle = !it.restart_required;

    it.available_actions = evaluate_action_policy(it);
    return it;
}

PluginDialogItem descriptor_to_item(const PluginDescriptor& d, const std::string& cur_build)
{
    bool loaded = false;
    std::string load_err;
    try { loaded = Slic3r::PluginManager::instance().is_plugin_loaded(d.id); } catch (...) {}
    try { load_err = Slic3r::PluginManager::instance().get_plugin_load_error(d.id); } catch (...) {}
    return descriptor_to_item(d, cur_build, loaded, load_err);
}

} // namespace Slic3r::GUI
