#include "orca/plugin/hook.hpp"
#include "orca/plugin/plugin.hpp"

#include <windows.h>
#include <shlobj.h>
#include <knownfolders.h>

#include <filesystem>
#include <fstream>
#include <mutex>
#include <string>
#include <cstdio>
#include <cstdint>

namespace {

// Plugin identity — must match PE resource metadata used by PackageReader.
constexpr const char* kPluginId = "com.orca.cpp-example";

// Manifest-derived targets with typed bindings and readable signatures:
//   Slic3r::Utils::get_current_time_utc — __int64 __cdecl Slic3r::Utils::get_current_time_utc(void)
//     RVA 9412016 (0x8F9DB0), typed_binding.available == true.
//     Called during CLI::run before help dispatch (global_begin_time), always hit in headless CLI mode (--info and --help).
//   Slic3r::Model::print_info — void __cdecl Slic3r::Model::print_info(void) const
//     RVA 686000 (0xA77B0), typed_binding.available == true.
//     Called during CLI --info path: OrcaSlicer.cpp:5663 for (Model &model : m_models) { model.print_info(); }
//     Therefore hit deterministic on orca-slicer.exe --info <file>
namespace orca_generated {
inline constexpr ::orca::plugin::HookTarget<void(*)()> kGetTimeUtc{"Slic3r::Utils::get_current_time_utc", 9412016};
inline constexpr ::orca::plugin::HookTarget<void(*)()> kPrintInfo{"Slic3r::Model::print_info", 686000};
} // namespace orca_generated

// Global host set in on_plugin_load for data_dir resolution via resolve_symbol.
static const orca_host_api_v1_t* g_plugin_host = nullptr;
static std::mutex g_log_mutex;

// Resolve data_dir via host resolve_symbol("Slic3r::data_dir") when available,
// falling back to %APPDATA%\OrcaSlicer and finally to a temp fallback.
static std::string resolve_data_dir() {
    // Primary: host get_data_dir (new in abi v1, size-versioned)
    if (g_plugin_host && g_plugin_host->get_data_dir) {
        const char* p = g_plugin_host->get_data_dir();
        if (p && p[0] != '\0') return std::string(p);
    }
    // Fallback: host resolve_symbol("Slic3r::data_dir") for older hosts
    if (g_plugin_host && g_plugin_host->resolve_symbol) {
        void* addr = nullptr;
        uint64_t rva = 0;
        if (g_plugin_host->resolve_symbol("Slic3r::data_dir", &addr, &rva) == ORCA_HOOK_OK && addr) {
            using DataDirFn = const std::string& (*)();
            auto fn = reinterpret_cast<DataDirFn>(addr);
            try {
                const std::string& s = fn();
                if (!s.empty()) return s;
            } catch (...) {
            }
        }
    }
    if (const char* env = std::getenv("ORCA_DATA_DIR")) {
        if (env[0] != '\0') return std::string(env);
    }
    PWSTR wpath = nullptr;
    if (SUCCEEDED(SHGetKnownFolderPath(FOLDERID_RoamingAppData, 0, nullptr, &wpath)) && wpath) {
        char mb[1024] = {0};
        int n = WideCharToMultiByte(CP_UTF8, 0, wpath, -1, mb, (int)sizeof(mb) - 1, nullptr, nullptr);
        CoTaskMemFree(wpath);
        if (n > 0) {
            std::string s(mb);
            if (!s.empty()) {
                std::filesystem::path p = std::filesystem::path(s) / "OrcaSlicer";
                return p.string();
            }
        } else {
            CoTaskMemFree(wpath);
        }
    }
    if (const char* env = std::getenv("APPDATA")) {
        std::filesystem::path p = std::filesystem::path(env) / "OrcaSlicer";
        return p.string();
    }
    return (std::filesystem::current_path() / "OrcaSlicer_data").string();
}

static void append_log(const char* hook_name, const std::string& observed) {
    std::lock_guard<std::mutex> lk(g_log_mutex);
    std::string data_dir = resolve_data_dir();
    std::filesystem::path log_path = std::filesystem::path(data_dir) / "orca_plugins" / "demo-logs" / "cpp-demo.log";
    std::error_code ec;
    std::filesystem::create_directories(log_path.parent_path(), ec);
    std::ofstream out(log_path, std::ios::app);
    if (!out) return;
    out << "plugin=" << kPluginId << " hook=" << hook_name << " observed=" << observed << "\n";
}

static std::string format_cpu_common(orca_cpu_context_t* ctx) {
    if (!ctx) return "ctx=null";
    char buf[512];
    std::snprintf(buf, sizeof(buf), "rip=0x%llx rsp=0x%llx rax=0x%llx rcx=0x%llx rdx=0x%llx r8=0x%llx r9=0x%llx rflags=0x%llx size=%u version=%u",
                  (unsigned long long)ctx->rip, (unsigned long long)ctx->rsp,
                  (unsigned long long)ctx->rax, (unsigned long long)ctx->rcx,
                  (unsigned long long)ctx->rdx, (unsigned long long)ctx->r8,
                  (unsigned long long)ctx->r9, (unsigned long long)ctx->rflags,
                  ctx->size, ctx->version);
    return std::string(buf);
}

// before hook on get_current_time_utc — high priority
#if defined(__clang__)
__attribute__((used))
#endif
orca_hook_status_t before_get_time_trampoline(orca_cpu_context_t* ctx,
                                                orca_hook_result_t* out_result,
                                                void* user_data) noexcept {
    (void)user_data;
    try {
        std::string obs = format_cpu_common(ctx);
        obs += " target=Slic3r::Utils::get_current_time_utc before";
        append_log("before_get_time_utc", obs);
        std::fputs("[orca-hook cpp before] ", stdout);
        std::fputs(obs.c_str(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    } catch (...) {
    }
    if (out_result) {
        out_result->size = sizeof(*out_result);
        out_result->version = ORCA_HOOK_ABI_VERSION;
        out_result->action = ORCA_HOOK_ACTION_CONTINUE;
    }
    return ORCA_HOOK_OK;
}

// after hook on get_current_time_utc — low priority
#if defined(__clang__)
__attribute__((used))
#endif
orca_hook_status_t after_get_time_trampoline(orca_cpu_context_t* ctx,
                                               orca_hook_result_t* out_result,
                                               void* user_data) noexcept {
    (void)user_data;
    try {
        std::string obs = format_cpu_common(ctx);
        // For after, rax holds return value (timestamp)
        char extra[128];
        std::snprintf(extra, sizeof(extra), " after return=%lld", (long long)(ctx ? ctx->rax : 0));
        obs += extra;
        append_log("after_get_time_utc", obs);
        std::fputs("[orca-hook cpp after] ", stdout);
        std::fputs(obs.c_str(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    } catch (...) {
    }
    if (out_result) {
        out_result->size = sizeof(*out_result);
        out_result->version = ORCA_HOOK_ABI_VERSION;
        out_result->action = ORCA_HOOK_ACTION_CONTINUE;
    }
    return ORCA_HOOK_OK;
}

// replace hook on Model::print_info — calls next, logs before/after with this pointer
#if defined(__clang__)
__attribute__((used))
#endif
orca_hook_status_t replace_print_info_trampoline(orca_cpu_context_t* ctx,
                                              orca_hook_result_t* out_result,
                                              orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept,
                                              void* user_data) noexcept {
    (void)user_data;
    try {
        std::string obs = format_cpu_common(ctx);
        obs += " enter Slic3r::Model::print_info";
        // this pointer in RCX for member function
        char extra[128];
        std::snprintf(extra, sizeof(extra), " this_rcx=0x%llx", (unsigned long long)(ctx ? ctx->rcx : 0));
        obs += extra;
        append_log("replace_print_info_entry", obs);
        std::fputs("[orca-hook cpp replace entry] ", stdout);
        std::fputs(obs.c_str(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    } catch (...) {}
    orca_hook_status_t st = ORCA_HOOK_OK;
    if (next) {
        st = next(ctx);
    }
    try {
        std::string obs = format_cpu_common(ctx);
        obs += " exit Slic3r::Model::print_info";
        char extra[128];
        std::snprintf(extra, sizeof(extra), " status=%d", (int)st);
        obs += extra;
        append_log("replace_print_info", obs);
        std::fputs("[orca-hook cpp replace] ", stdout);
        std::fputs(obs.c_str(), stdout);
        std::fputs("\n", stdout);
        std::fflush(stdout);
    } catch (...) {
    }
    if (out_result) {
        out_result->size = sizeof(*out_result);
        out_result->version = ORCA_HOOK_ABI_VERSION;
        out_result->action = ORCA_HOOK_ACTION_CONTINUE;
    }
    return st;
}

// Manual registration via static initializers.
// Use __attribute__((used)) to prevent linker discarding in Release with clang-cl (hook-examples uses Ninja with /OPT:REF).
struct BeforeReg {
    BeforeReg() noexcept {
        ::orca::plugin::detail::HookRegistration r{};
        r.hook_id = "cpp.before_get_time";
        r.symbol_id = orca_generated::kGetTimeUtc.symbol_id;
        r.rva = orca_generated::kGetTimeUtc.rva;
        r.point = ::orca::plugin::HookPoint::Entry;
        r.kind = ::orca::plugin::HookKind::Before;
        r.priority = 1300;
        r.trampoline = (void*)before_get_time_trampoline;
        ::orca::plugin::detail::register_hook(r);
    }
};
#if defined(__clang__) || defined(__GNUC__)
__attribute__((used)) static BeforeReg g_before_reg;
#else
static BeforeReg g_before_reg;
#endif

struct AfterReg {
    AfterReg() noexcept {
        ::orca::plugin::detail::HookRegistration r{};
        r.hook_id = "cpp.after_get_time";
        r.symbol_id = orca_generated::kGetTimeUtc.symbol_id;
        r.rva = orca_generated::kGetTimeUtc.rva;
        r.point = ::orca::plugin::HookPoint::Return;
        r.kind = ::orca::plugin::HookKind::After;
        r.priority = 700;
        r.trampoline = (void*)after_get_time_trampoline;
        ::orca::plugin::detail::register_hook(r);
    }
};
#if defined(__clang__) || defined(__GNUC__)
__attribute__((used)) static AfterReg g_after_reg;
#else
static AfterReg g_after_reg;
#endif

struct ReplaceReg {
    ReplaceReg() noexcept {
        ::orca::plugin::detail::HookRegistration r{};
        r.hook_id = "cpp.replace_print_info";
        r.symbol_id = orca_generated::kPrintInfo.symbol_id;
        r.rva = orca_generated::kPrintInfo.rva;
        r.point = ::orca::plugin::HookPoint::Entry;
        r.kind = ::orca::plugin::HookKind::Replace;
        r.priority = 1150;
        r.trampoline = (void*)replace_print_info_trampoline;
        ::orca::plugin::detail::register_hook(r);
    }
};
#if defined(__clang__) || defined(__GNUC__)
__attribute__((used)) static ReplaceReg g_replace_reg;
#else
static ReplaceReg g_replace_reg;
#endif

} // anonymous namespace

extern "C" int on_plugin_load(const orca_host_api_v1_t* host) noexcept {
    g_plugin_host = host;
    try {
        std::ofstream dbg("C:/Users/User/CLionProjects/OrcaSlicer/cpp_debug_load.log", std::ios::app);
        if (dbg) dbg << "on_plugin_load called host=" << (void*)host << " plugin=" << kPluginId << "\n";
    } catch (...) {}
    try {
        append_log("on_load", "plugin loaded");
    } catch (...) {
    }
    return 0;
}
extern "C" int on_plugin_unload(const orca_host_api_v1_t* host) noexcept {
    (void)host;
    try {
        append_log("on_unload", "plugin unloading");
    } catch (...) {
    }
    g_plugin_host = nullptr;
    return 0;
}
