#ifndef slic3r_PluginsDialog_hpp_
#define slic3r_PluginsDialog_hpp_

#include "Widgets/WebViewHostDialog.hpp"
#include "PluginStatus.hpp"
#include "PluginSort.hpp"
#include <atomic>
#include <exception>
#include <functional>
#include <memory>
#include <mutex>
#include <optional>
#include <string>
#include <thread>
#include <type_traits>
#include <utility>
#include <wx/evtloop.h>
#include <wx/app.h>
#include <wx/progdlg.h>
#include <wx/string.h>
#include <wx/timer.h>

class wxTimer;

namespace Slic3r {
namespace GUI {

class PluginsDialog : public Slic3r::GUI::WebViewHostDialog
{
public:
    PluginsDialog(wxWindow* parent,
                  wxWindowID id         = wxID_ANY,
                  const wxString& title = wxT(""),
                  const wxPoint& pos    = wxDefaultPosition,
                  const wxSize& size    = wxDefaultSize,
                  long style            = wxSYSTEM_MENU | wxCAPTION | wxCLOSE_BOX | wxMAXIMIZE_BOX | wxRESIZE_BORDER);

    ~PluginsDialog();

    void update_plugin_dialog_ui();

private:
    void on_script_message(const nlohmann::json& payload) override;
    void handle_web_command(const nlohmann::json& payload);
    void restore_z_order();

    void send_plugins();
    void set_plugin_sort(const std::string& sort_key, const std::string& sort_order);
    nlohmann::json build_plugins_payload() const;

    void refresh_plugins();
    void toggle_plugin(const std::string& plugin_id, bool enabled);
    void handle_plugin_menu_action(const std::string& plugin_id, const std::string& action);

    void install_plugin_from_file();
    bool install_plugin_package(const std::string& package_path);

    void open_plugin_folder(const std::string& plugin_id);
    void delete_plugin(const std::string& plugin_id);
    void reload_plugin(const std::string& plugin_id);

    void show_status(const wxString& message, const char* level);
    wxString plugin_display_name(const std::string& plugin_id) const;

    template<typename Run, typename OnFinish>
    void run_with_dialog(Run&& run,
                         OnFinish&& on_finish,
                         const wxString& title,
                         const wxString& message,
                         int maximum = 100,
                         int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE,
                         bool finish_after_dialog_destroyed = false)
    {
        const auto alive = m_alive;
        wxProgressDialog* progress = new wxProgressDialog(title, message, maximum, this, style);
        wxTimer* timer             = new wxTimer();

        timer->Bind(wxEVT_TIMER, [alive, progress, message](wxTimerEvent&) {
            if (alive->load(std::memory_order_acquire) && progress)
                progress->Pulse(message);
        });

        timer->Start(100);

        std::thread([this,
                     alive,
                     progress,
                     timer,
                     run       = std::forward<Run>(run),
                     on_finish = std::forward<OnFinish>(on_finish),
                     finish_after_dialog_destroyed]() mutable {
            try {
                run();
            } catch (const std::exception& ex) {
                BOOST_LOG_TRIVIAL(error) << "Plugin dialog worker failed: " << ex.what();
            } catch (...) {
                BOOST_LOG_TRIVIAL(error) << "Plugin dialog worker failed with an unknown exception";
            }

            if (wxTheApp == nullptr)
                return;

            wxTheApp->CallAfter([this,
                                 alive,
                                 progress,
                                 timer,
                                 on_finish = std::move(on_finish),
                                 finish_after_dialog_destroyed]() mutable {
                timer->Stop();
                delete timer;

                if (alive->load(std::memory_order_acquire)) {
                    progress->Destroy();
                    restore_z_order();
                    on_finish();
                } else if (finish_after_dialog_destroyed) {
                    on_finish();
                }
            });
        }).detach();
    }

    template<typename Run>
    std::invoke_result_t<std::decay_t<Run>&> run_with_dialog_wait(Run&& run,
                                                                  const wxString& title,
                                                                  const wxString& message,
                                                                  int maximum = 100,
                                                                  int style   = wxPD_APP_MODAL | wxPD_AUTO_HIDE)
    {
        using Result = std::invoke_result_t<std::decay_t<Run>&>;

        bool finished = false;
        wxEventLoop loop;
        auto on_finish = [&finished, &loop]() {
            finished = true;
            if (loop.IsRunning())
                loop.Exit();
        };

        if constexpr (std::is_void_v<Result>) {
            struct WaitState
            {
                std::mutex mutex;
                std::exception_ptr exception;
            };

            auto state = std::make_shared<WaitState>();
            run_with_dialog(
                [run = std::forward<Run>(run), state]() mutable {
                    try {
                        run();
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->exception = std::current_exception();
                    }
                },
                on_finish, title, message, maximum, style, true);

            if (!finished)
                loop.Run();

            std::exception_ptr exception;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                exception = state->exception;
            }
            if (exception)
                std::rethrow_exception(exception);
        } else {
            using StoredResult = std::decay_t<Result>;
            struct WaitState
            {
                std::mutex mutex;
                std::optional<StoredResult> result;
                std::exception_ptr exception;
            };

            auto state = std::make_shared<WaitState>();
            run_with_dialog(
                [run = std::forward<Run>(run), state]() mutable {
                    try {
                        StoredResult result = run();
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->result.emplace(std::move(result));
                    } catch (...) {
                        std::lock_guard<std::mutex> lock(state->mutex);
                        state->exception = std::current_exception();
                    }
                },
                on_finish, title, message, maximum, style, true);

            if (!finished)
                loop.Run();

            std::optional<StoredResult> result;
            std::exception_ptr exception;
            {
                std::lock_guard<std::mutex> lock(state->mutex);
                if (state->result)
                    result.emplace(std::move(*state->result));
                exception = state->exception;
            }
            if (exception)
                std::rethrow_exception(exception);
            return std::move(*result);
        }
    }

    std::shared_ptr<std::atomic<bool>> m_alive = std::make_shared<std::atomic<bool>>(true);
    PluginSortKey m_plugin_sort_key       = PluginSortKey::None;
    PluginSortOrder m_plugin_sort_order   = PluginSortOrder::Asc;
    std::string m_activating_plugin_id;
};

} // namespace GUI
} // namespace Slic3r

#endif
