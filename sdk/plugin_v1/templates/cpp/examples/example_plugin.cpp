#include "orca/plugin/plugin.hpp"
#include "orca/plugin/hook.hpp"
#include "orca/plugin/raw.hpp"
#include "orca/generated/symbols.hpp"

namespace orca_generated {
inline constexpr ::orca::plugin::HookTarget<void(*)()> kPrintHelp{"Slic3r::CLI::print_help", 0};
}
namespace {
void before_print_help() noexcept {
    const char mark[] = "[orca-hook cpp before]\n";
    (void)mark;
}
void after_print_help() noexcept {
    const char mark[] = "[orca-hook cpp after]\n";
    (void)mark;
}
}
ORCA_BEFORE_PRIO(orca_generated::kPrintHelp, before_print_help, 1300)
ORCA_AFTER_PRIO(orca_generated::kPrintHelp, after_print_help, 1300)
extern "C" int on_plugin_load(const orca_host_api_v1_t* host) noexcept { (void)host; return 0; }
extern "C" int on_plugin_unload(const orca_host_api_v1_t* host) noexcept { (void)host; return 0; }
