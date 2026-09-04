#include "PluginsDialog.hpp"

#include "GUI.hpp"
#include "GUI_App.hpp"
#include "I18N.hpp"
#include "PluginDialogPayload.hpp"

#include <libslic3r/Utils.hpp>
#include <slic3r/GUI/format.hpp>

#include <boost/filesystem.hpp>
#include <boost/log/trivial.hpp>
#include <boost/nowide/fstream.hpp>

#include <slic3r/plugin/package/PackageReader.hpp>
#include <slic3r/plugin/package/PluginMetadata.hpp>
#include <slic3r/plugin/package/InstallState.hpp>
#include <slic3r/plugin/package/Hash.hpp>
#include <slic3r/plugin/PluginManager.hpp>

#include <algorithm>
#include <memory>
#include <mutex>
#include <sstream>
#include <iomanip>
#include <vector>

#include <wx/dialog.h>
#include <wx/event.h>
#include <wx/filedlg.h>
#include <wx/msgdlg.h>
#include <wx/progdlg.h>
#include <wx/timer.h>
#include <wx/utils.h>

namespace Slic3r { namespace GUI {
namespace {

const wxString kDeletePluginTitle = _L("Delete Plugin");

std::vector<PluginDialogItem> scan_local_plugins()
{
    std::string cur_build;
    try { cur_build = Slic3r::PluginManager::instance().current_build_id(); } catch (...) {}
    if (cur_build.empty()) cur_build = get_current_build_id_fallback();

    std::vector<PluginDialogItem> items;
    try {
        auto descs = Slic3r::PluginManager::instance().get_plugin_descriptors(true);
        items.reserve(descs.size());
        for (auto &d : descs) {
            items.push_back(descriptor_to_item(d, cur_build));
        }
        if (items.empty()) {
            boost::filesystem::path base = boost::filesystem::path(Slic3r::data_dir()) / "orca_plugins";
            if (boost::filesystem::exists(base) && boost::filesystem::is_directory(base)) {
                for (boost::filesystem::directory_iterator it(base), end; it!=end; ++it) {
                    if (!boost::filesystem::is_directory(it->path())) continue;
                    std::string name = it->path().filename().string();
                    if (name.empty() || name[0]=='.' || name[0]=='_') continue;
                    PluginDescriptor dd;
                    if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(name, dd)) {
                        items.push_back(descriptor_to_item(dd, cur_build));
                    }
                }
            }
        }
    } catch (const std::exception& ex) {
        BOOST_LOG_TRIVIAL(error) << "scan_local_plugins failed: " << ex.what();
    }
    return items;
}

struct PluginOpState
{
    std::mutex mutex;
    bool succeeded = false;
    std::string error;
};

void store_op(const std::shared_ptr<PluginOpState>& s, bool ok, std::string e){ std::lock_guard<std::mutex> lk(s->mutex); s->succeeded=ok; s->error=std::move(e); }
bool take_op(const std::shared_ptr<PluginOpState>& s, std::string &e){ std::lock_guard<std::mutex> lk(s->mutex); e=std::move(s->error); return s->succeeded; }

std::string normalize_error(const std::string& e){ if(e.empty()) return "Unknown error."; return e; }

} // namespace

PluginsDialog::PluginsDialog(wxWindow* parent, wxWindowID id, const wxString&, const wxPoint& pos, const wxSize& size, long style)
    : WebViewHostDialog(parent, id, _L("Plugins"), pos, size, style)
{ create_webview("web/dialog/PluginsDialog/index.html", _L("Plugins"), wxSize(980, 760), wxSize(760, 600)); }

PluginsDialog::~PluginsDialog() { m_alive->store(false, std::memory_order_release); }

void PluginsDialog::update_plugin_dialog_ui()
{
    send_plugins();
}

void PluginsDialog::on_script_message(const nlohmann::json& payload)
{
    if (handle_common_script_command(payload))
        return;
    wxGetApp().CallAfter([this, alive = m_alive, payload]() {
        if (alive->load(std::memory_order_acquire))
            handle_web_command(payload);
    });
}

void PluginsDialog::handle_web_command(const nlohmann::json& payload)
{
    const std::string command = payload.value("command", "");
    if (command == "request_plugins") {
        send_plugins();
    } else if (command == "refresh_plugins") {
        refresh_plugins();
    } else if (command == "toggle_plugin") {
        toggle_plugin(payload.value("id", ""), payload.value("enabled", false));
    } else if (command == "install_local_plugin") {
        install_plugin_from_file();
    } else if (command == "plugin_menu_action") {
        handle_plugin_menu_action(payload.value("id", ""), payload.value("action", ""));
    } else if (command == "set_plugin_sort") {
        set_plugin_sort(payload.value("sort_key", ""), payload.value("sort_order", ""));
    }
}

void PluginsDialog::send_plugins() { call_web_handler(build_plugins_payload()); }

void PluginsDialog::set_plugin_sort(const std::string& sort_key, const std::string& sort_order)
{
    m_plugin_sort_key   = plugin_sort_key_from_string(sort_key, m_plugin_sort_key);
    m_plugin_sort_order = plugin_sort_order_from_string(sort_order, m_plugin_sort_order);
    send_plugins();
}

nlohmann::json PluginsDialog::build_plugins_payload() const
{
    nlohmann::json response;
    response["command"]    = "list_plugins";
    response["sort_key"]   = to_string(m_plugin_sort_key);
    response["sort_order"] = to_string(m_plugin_sort_order);
    std::string cur;
    try { cur = Slic3r::PluginManager::instance().current_build_id(); } catch (...) {}
    if (cur.empty()) cur = get_current_build_id_fallback();
    response["current_build_id"] = cur;
    response["data"]       = nlohmann::json::array();

    std::vector<PluginDialogItem> items = scan_local_plugins();
    sort_plugin_items_for_dialog(items, m_plugin_sort_key, m_plugin_sort_order);
    for (auto &it : items) response["data"].push_back(build_plugin_payload_item(it));
    BOOST_LOG_TRIVIAL(info) << "PluginsDialog prepared " << items.size() << " rows";
    return response;
}

void PluginsDialog::refresh_plugins()
{
    BOOST_LOG_TRIVIAL(info) << "Refreshing local plugins";
    auto state = std::make_shared<PluginOpState>();
    run_with_dialog([state](){
        try { Slic3r::PluginManager::instance().rescan_plugins(); } catch (...) {}
        store_op(state, true, "");
    }, [this, state](){
        std::string err; take_op(state, err);
        send_plugins();
        if (!err.empty()) show_status(from_u8(err), "warn");
        else show_status(_L("Refreshed."), "success");
    }, _L("Refreshing"), _L("Refreshing plugins"));
}

void PluginsDialog::toggle_plugin(const std::string& plugin_id, bool enabled)
{
    if (plugin_id.empty()) return;
    BOOST_LOG_TRIVIAL(info) << "Toggle plugin " << plugin_id << " enabled=" << enabled;
    try {
        PluginDescriptor d;
        if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(plugin_id, d)) {
            std::string cur;
            try { cur = Slic3r::PluginManager::instance().current_build_id(); } catch (...) {}
            if (cur.empty()) cur = get_current_build_id_fallback();
            Slic3r::Plugin::Package::PluginMetadata tmp; tmp.targets = d.targets;
            bool compat = d.metadata_valid && Slic3r::Plugin::Package::has_exact_build_match(tmp, cur);
            if (!compat) {
                show_status(wxString::Format(_L("Plugin \"%s\" is incompatible and cannot be activated."), from_u8(d.name.empty()?plugin_id:d.name)), "error");
                send_plugins(); return;
            }
            if (d.restart_required) {
                show_status(_L("Restart required before toggling this plugin."), "warn");
                send_plugins(); return;
            }
        }
    } catch (...) {}

    auto state = std::make_shared<PluginOpState>();
    run_with_dialog([plugin_id, enabled, state](){
        std::string err;
        bool ok=false;
        try {
            if (enabled) {
                if (!Slic3r::PluginManager::instance().set_plugin_enabled(plugin_id, true, err)) {
                    store_op(state,false,err); return;
                }
                if (!Slic3r::PluginManager::instance().load_plugin(plugin_id, err)) {
                    store_op(state,false,err); return;
                }
                store_op(state,true,"");
            } else {
                if (!Slic3r::PluginManager::instance().unload_plugin(plugin_id, err)) {
                    // unload may set restart_required but still return false
                    store_op(state,false,err); return;
                }
                std::string ee;
                Slic3r::PluginManager::instance().set_plugin_enabled(plugin_id, false, ee);
                store_op(state,true,"");
            }
        } catch (const std::exception& ex) { store_op(state,false,ex.what()); }
    }, [this, state, plugin_id, enabled](){
        std::string err; bool ok = take_op(state, err);
        send_plugins();
        if (!ok) {
            show_status(from_u8(normalize_error(err)), "error");
            return;
        }
        wxString name = plugin_display_name(plugin_id);
        show_status(wxString::Format(enabled ? _L("Enabled \"%s\".") : _L("Disabled \"%s\"."), name), "success");
    }, enabled ? _L("Enabling plugin") : _L("Disabling plugin"), enabled ? _L("Enabling plugin") : _L("Disabling plugin"));
}

void PluginsDialog::handle_plugin_menu_action(const std::string& plugin_id, const std::string& action)
{
    if (action == "open_folder") open_plugin_folder(plugin_id);
    else if (action == "delete_plugin") delete_plugin(plugin_id);
    else if (action == "reload_plugin") reload_plugin(plugin_id);
}

void PluginsDialog::restore_z_order()
{
    wxGetApp().CallAfter([this, alive = m_alive]() {
        if (alive->load(std::memory_order_acquire) && IsShown())
            Raise();
    });
}

void PluginsDialog::install_plugin_from_file()
{
#ifdef _WIN32
    const wxString filter = _L("Plugin files (*.dll;*.jar)|*.dll;*.jar");
#else
    const wxString filter = _L("Plugin files (*.so;*.jar)|*.so;*.jar");
#endif
    wxFileDialog dialog(this, _L("Select plugin package"), wxEmptyString, wxEmptyString, filter, wxFD_OPEN | wxFD_FILE_MUST_EXIST);
    const int rc = dialog.ShowModal();
    restore_z_order();
    if (rc != wxID_OK) return;
    if (!install_plugin_package(dialog.GetPath().ToUTF8().data())) {
        BOOST_LOG_TRIVIAL(warning) << "Failed to install plugin package";
    }
}

bool PluginsDialog::install_plugin_package(const std::string& package_path)
{
    if (package_path.empty()) return false;
    boost::filesystem::path src(package_path);
    std::string ext = src.extension().string();
    std::transform(ext.begin(), ext.end(), ext.begin(), ::tolower);
#ifdef _WIN32
    if (ext != ".dll" && ext != ".jar") { show_status(_L("Select a .dll or .jar plugin package."), "info"); return false; }
#else
    if (ext != ".so" && ext != ".jar") { show_status(_L("Select a .so or .jar plugin package."), "info"); return false; }
#endif

    auto insp = Slic3r::Plugin::Package::inspect_plugin_file(src);
    if (!insp.ok) {
        show_status(from_u8(insp.error.empty() ? "Invalid plugin package." : insp.error), "error");
        send_plugins();
        return false;
    }

    try {
        PluginDescriptor existing;
        if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(insp.metadata.id, existing) && existing.restart_required) {
            show_status(_L("Cannot overwrite plugin that requires restart. Please restart OrcaSlicer first."), "error");
            return false;
        }
        if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(insp.metadata.id, existing)) {
            wxString name = from_u8(insp.metadata.name.empty() ? insp.metadata.id : insp.metadata.name);
            wxMessageDialog dlg(this, wxString::Format(_L("Plugin \"%s\" is already installed.\n\nInstalling this package will overwrite the existing plugin."), name),
                                _L("Overwrite Plugin"), wxOK | wxCANCEL | wxICON_WARNING);
            dlg.SetOKCancelLabels(_L("Overwrite"), _L("Cancel"));
            int r = dlg.ShowModal();
            restore_z_order();
            if (r != wxID_OK) return false;
        }
    } catch (...) {}

    auto state = std::make_shared<PluginOpState>();
    std::string filename = src.filename().string();
    bool ok=false;
    std::string op_err;
    try {
        ok = run_with_dialog_wait([src, state]() -> bool {
            std::string err;
            PluginDescriptor out;
            bool res = Slic3r::PluginManager::instance().install_plugin(src, out, err);
            if (!res) { store_op(state,false,err); return false; }
            store_op(state,true,"");
            return true;
        }, _L("Installing plugin"), _L("Installing plugin") + ": " + from_u8(filename));
        std::string serr; take_op(state, serr);
        if (!serr.empty()) op_err = serr;
    } catch (const std::exception& ex) { op_err = ex.what(); ok=false; }
    if (!ok) {
        show_status(from_u8(op_err.empty() ? "Failed to install plugin package." : op_err), "error");
        send_plugins();
        return false;
    }
    try {
        PluginDescriptor d;
        if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(insp.metadata.id, d)) {
            std::string cur;
            try { cur = Slic3r::PluginManager::instance().current_build_id(); } catch (...) {}
            Slic3r::Plugin::Package::PluginMetadata tmp; tmp.targets=d.targets;
            bool compat = d.metadata_valid && Slic3r::Plugin::Package::has_exact_build_match(tmp, cur);
            if (compat && d.enabled) {
                std::string le;
                Slic3r::PluginManager::instance().load_plugin(d.id, le);
                if (!le.empty()) BOOST_LOG_TRIVIAL(warning) << "Auto-load after install failed: " << le;
            }
        }
    } catch (...) {}
    show_status(wxString::Format(_L("Installed \"%s\"."), from_u8(insp.metadata.id)), "success");
    send_plugins();
    return true;
}

wxString PluginsDialog::plugin_display_name(const std::string& plugin_id) const
{
    try {
        PluginDescriptor d;
        if (Slic3r::PluginManager::instance().try_get_plugin_descriptor(plugin_id, d) && !d.name.empty())
            return from_u8(d.name);
    } catch (...) {}
    return from_u8(plugin_id);
}

void PluginsDialog::show_status(const wxString& message, const char* level)
{
    nlohmann::json payload;
    payload["command"] = "status_message";
    payload["level"]   = level;
    payload["message"] = into_u8(message);
    call_web_handler(payload);
}

void PluginsDialog::open_plugin_folder(const std::string& plugin_id)
{
    if (plugin_id.empty()) { show_status(_L("Plugin id is empty."), "warn"); return; }
    try { Slic3r::PluginManager::instance().open_plugin_folder(plugin_id); }
    catch (const std::exception& ex) { show_status(from_u8(ex.what()), "warn"); }
}

void PluginsDialog::delete_plugin(const std::string& plugin_id)
{
    if (plugin_id.empty()) return;
    wxString name = plugin_display_name(plugin_id);
    int rc = wxMessageBox(wxString::Format(_L("Delete plugin \"%s\"?\n\nThis permanently removes the plugin folder."), name),
                          kDeletePluginTitle, wxYES_NO | wxNO_DEFAULT | wxICON_WARNING, this);
    restore_z_order();
    if (rc != wxYES) return;
    auto state = std::make_shared<PluginOpState>();
    run_with_dialog([plugin_id, state](){
        std::string err;
        bool ok = Slic3r::PluginManager::instance().delete_plugin(plugin_id, err);
        if (!ok) store_op(state,false,err);
        else store_op(state,true,"");
    }, [this, state, name](){
        std::string err; bool ok = take_op(state, err);
        if (!ok) { show_status(from_u8(err.empty() ? "Failed to delete plugin." : err), "error"); send_plugins(); return; }
        send_plugins();
        show_status(wxString::Format(_L("Deleted \"%s\"."), name), "success");
    }, _L("Deleting plugin"), _L("Deleting plugin..."));
}

void PluginsDialog::reload_plugin(const std::string& plugin_id)
{
    if (plugin_id.empty()) return;
    auto state = std::make_shared<PluginOpState>();
    run_with_dialog([plugin_id, state](){
        std::string err;
        bool ok = Slic3r::PluginManager::instance().reload_plugin(plugin_id, err);
        if (!ok) store_op(state,false,err);
        else store_op(state,true,"");
    }, [this, state, plugin_id](){
        std::string err; bool ok = take_op(state, err);
        send_plugins();
        if (!ok) show_status(from_u8(err.empty() ? "Failed to reload plugin." : err), "warn");
        else show_status(wxString::Format(_L("Reloaded \"%s\"."), plugin_display_name(plugin_id)), "success");
    }, _L("Reloading plugin"), _L("Reloading plugin"));
}

}} // namespace Slic3r::GUI
