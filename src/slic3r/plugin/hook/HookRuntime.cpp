#include "HookRuntime.hpp"
#include "HookBackendWindows.hpp"
#include <algorithm>
#include <chrono>
#include <cstring>
#include <thread>

#include <filesystem>
#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#endif
namespace Slic3r { const std::string& data_dir(); }
namespace Slic3r::Hook {
namespace {
thread_local std::string t_lastHookError;
}


HookRuntime& HookRuntime::Instance() {
    static HookRuntime inst;
    return inst;
}

bool HookRuntime::Initialize(const std::string& runtime_manifest_path, std::string& error) {
    if (initialized_) return true;
    backend_ = HookBackend::CreateForCurrentPlatform();
    active_build_ = backend_->ActiveBuildId();
    {
        auto hex = [](const uint8_t* d, size_t n){ std::string s; s.reserve(n*2); const char* h="0123456789abcdef"; for(size_t i=0;i<n;++i){ s.push_back(h[(d[i]>>4)&0xF]); s.push_back(h[d[i]&0xF]); } return s; };
        std::string active_hash = hex(active_build_.image_hash, 32);
        std::string active_guid = hex(active_build_.debug_guid, 16);
        fprintf(stderr, "[HookRuntime] Initialize: active build os=%u arch=%u age=%u guid=%s hash=%s file=%s\n", (unsigned)active_build_.os, (unsigned)active_build_.arch, (unsigned)active_build_.debug_age, active_guid.c_str(), active_hash.c_str(), active_build_.debug_file.c_str());
    }
    manifest_ = std::make_unique<HookManifest>();
    std::string manifest_path = runtime_manifest_path;
    if (manifest_path.empty()) {
        manifest_path = FindDefaultManifestPath();
    }
    fprintf(stderr, "[HookRuntime] manifest_path=%s\n", manifest_path.c_str());
    if (!manifest_path.empty()){
        bool has_manifest = false;
        std::string e;
        if (manifest_->LoadFromBinaryFile(manifest_path, e)){
            has_manifest = true;
        } else {
            if (manifest_->LoadFromJsonFile(manifest_path, e)) has_manifest = true;
        }
        if (!has_manifest){
            last_error_ = "Failed to load manifest '" + manifest_path + "': " + e;
            t_lastHookError = last_error_;
            fprintf(stderr, "[HookRuntime] manifest load failed: %s\n", last_error_.c_str());
        } else {
            auto hex = [](const uint8_t* d, size_t n){ std::string s; s.reserve(n*2); const char* h="0123456789abcdef"; for(size_t i=0;i<n;++i){ s.push_back(h[(d[i]>>4)&0xF]); s.push_back(h[d[i]&0xF]); } return s; };
            std::string m_hash = hex(manifest_->build.image_hash, 32);
            std::string m_guid = hex(manifest_->build.debug_guid, 16);
            fprintf(stderr, "[HookRuntime] manifest build os=%u arch=%u age=%u guid=%s hash=%s file=%s\n", (unsigned)manifest_->build.os, (unsigned)manifest_->build.arch, (unsigned)manifest_->build.debug_age, m_guid.c_str(), m_hash.c_str(), manifest_->build.debug_file.c_str());
            std::string ve;
            if (!manifest_->ValidateBuildId(active_build_, ve)){
                error = "BuildId mismatch for manifest '" + manifest_path + "': " + ve;
                last_error_ = error;
                fprintf(stderr, "[HookRuntime] ValidateBuildId failed: %s manifest_hash=%s active_hash=%s manifest_guid=%s active_guid=%s manifest_file='%s' active_file='%s' os %u vs %u arch %u vs %u age %u vs %u\n",
                    ve.c_str(), m_hash.c_str(), hex(active_build_.image_hash,32).c_str(), m_guid.c_str(), hex(active_build_.debug_guid,16).c_str(),
                    manifest_->build.debug_file.c_str(), active_build_.debug_file.c_str(),
                    (unsigned)manifest_->build.os, (unsigned)active_build_.os, (unsigned)manifest_->build.arch, (unsigned)active_build_.arch,
                    (unsigned)manifest_->build.debug_age, (unsigned)active_build_.debug_age);
                return false;
            }
        }
    } else {
        last_error_ = "No manifest path provided and auto-locate failed; continuing with empty manifest (ValidateBuildId will fail on install)";
        fprintf(stderr, "[HookRuntime] %s\n", last_error_.c_str());
    }
    registry_ = std::make_unique<HookRegistry>(manifest_.get(), backend_.get());
    dispatcher_ = std::make_unique<Dispatcher>(registry_.get());
    initialized_ = true;
    fprintf(stderr, "[HookRuntime] Initialize succeeded\n");
    return true;
}

void HookRuntime::Shutdown() noexcept {
    if (!initialized_) return;
    try {
        if (registry_) {
            // Snapshot first: RemovePluginHooks mutates the hook lists being iterated.
            std::vector<PluginId> plugins;
            for (auto& ch : registry_->AllChains()) {
                std::vector<std::shared_ptr<HookRequest>> hs;
                registry_->SnapshotHooks(ch, hs);
                for (auto& h : hs) {
                    if (std::find(plugins.begin(), plugins.end(), h->plugin_id) == plugins.end())
                        plugins.push_back(h->plugin_id);
                }
            }
            for (auto& pid : plugins){
                std::string e;
                registry_->RemovePluginHooks(pid, 1500, e);
            }
        }
    } catch (...) {}
    dispatcher_.reset();
    registry_.reset();
    manifest_.reset();
    backend_.reset();
    initialized_ = false;
}

bool HookRuntime::ValidateBuildId(std::string& error) const {
    if (!manifest_ || manifest_->Empty()){
        error = "no manifest loaded (build with ORCA_GENERATE_HOOK_SDK=ON to produce orca-hooks.bin)";
        return false;
    }
    return manifest_->ValidateBuildId(active_build_, error);
}

bool HookRuntime::InstallPluginHooks(const PluginId& plugin_id,
                                      std::vector<std::shared_ptr<HookRequest>> requests,
                                      std::string& error) {
    if (!initialized_){
        error="runtime not initialized";
        return false;
    }
    if (!backend_ || !backend_->IsSupported()){
        error="hook backend not supported on this platform";
        return false;
    }
    {
        std::string ve;
        if (!ValidateBuildId(ve)){
            error = ve;
            return false;
        }
    }
    for (size_t i=0;i<requests.size();++i){
        if (requests[i]->declare_order==0) requests[i]->declare_order = static_cast<uint32_t>(i);
        if (requests[i]->plugin_id.empty()) requests[i]->plugin_id = plugin_id;
        if (requests[i]->hook_id.empty()){
            requests[i]->hook_id = plugin_id + ":" + requests[i]->target_id + ":" + std::to_string(requests[i]->declare_order);
        }
    }
    return registry_->InstallTransaction(plugin_id, std::move(requests), error);
}

HookStatus HookRuntime::RemovePluginHooks(const PluginId& plugin_id, uint32_t timeout_ms, std::string& error){
    if (!registry_) { error="no registry"; return HookStatus::InternalError; }
    return registry_->RemovePluginHooks(plugin_id, timeout_ms, error);
}

bool HookRuntime::ReadMemory(const void* src, void* dst, size_t sz) const noexcept {
    if (backend_) return backend_->ReadMemory(src,dst,sz);
    return HookMemory::Read(src,dst,sz);
}
bool HookRuntime::WriteMemory(void* dst, const void* src, size_t sz) const noexcept {
    if (backend_) return backend_->WriteMemory(dst,src,sz);
    return HookMemory::Write(dst,src,sz);
}
bool HookRuntime::ProtectMemory(void* addr, size_t sz, MemProt prot, MemProt* old) const noexcept {
    if (backend_) return backend_->ProtectMemory(addr,sz,prot,old);
    return HookMemory::Protect(addr,sz,prot,old);
}
void HookRuntime::FlushICache(void* addr, size_t sz) const noexcept {
    if (backend_) backend_->FlushICache(addr,sz);
    else HookMemory::FlushICache(addr,sz);
}
void HookRuntime::SetLastHookError(const std::string& err) { t_lastHookError = err; }
std::string HookRuntime::GetLastHookError() { return t_lastHookError; }
const char* HookRuntime::HookStatusName(orca_hook_status_t status) noexcept {
    switch (status) {
        case ORCA_HOOK_OK: return "OK";
        case ORCA_HOOK_ERR_INVALID_ARG: return "INVALID_ARG";
        case ORCA_HOOK_ERR_INVALID_SIZE: return "INVALID_SIZE";
        case ORCA_HOOK_ERR_UNSUPPORTED_ABI: return "UNSUPPORTED_ABI";
        case ORCA_HOOK_ERR_BUILD_MISMATCH: return "BUILD_MISMATCH";
        case ORCA_HOOK_ERR_NOT_FOUND: return "NOT_FOUND";
        case ORCA_HOOK_ERR_ALREADY_EXISTS: return "ALREADY_EXISTS";
        case ORCA_HOOK_ERR_RESOLVE_FAILED: return "RESOLVE_FAILED";
        case ORCA_HOOK_ERR_PATCH_FAILED: return "PATCH_FAILED";
        case ORCA_HOOK_ERR_BAD_INSTRUCTION_BOUNDARY: return "BAD_INSTRUCTION_BOUNDARY";
        case ORCA_HOOK_ERR_BAD_RVA: return "BAD_RVA";
        case ORCA_HOOK_ERR_VTABLE_BOUNDS: return "VTABLE_BOUNDS";
        case ORCA_HOOK_ERR_IMPORT_NOT_FOUND: return "IMPORT_NOT_FOUND";
        case ORCA_HOOK_ERR_PROTECT_FAILED: return "PROTECT_FAILED";
        case ORCA_HOOK_ERR_BUSY: return "BUSY";
        case ORCA_HOOK_ERR_RESTART_REQUIRED: return "RESTART_REQUIRED";
        case ORCA_HOOK_ERR_JVM_UNAVAILABLE: return "JVM_UNAVAILABLE";
        case ORCA_HOOK_ERR_INTERNAL: return "INTERNAL";
        default: return "UNKNOWN";
    }
}
std::string HookRuntime::FindDefaultManifestPath() {
    namespace fs = std::filesystem;
    std::vector<fs::path> candidates;
#ifdef _WIN32
    HMODULE hMod = nullptr;
    if (GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                          reinterpret_cast<LPCWSTR>(&FindDefaultManifestPath), &hMod) && hMod) {
        wchar_t wpath[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameW(hMod, wpath, MAX_PATH);
        if (len > 0) {
            fs::path modPath(wpath);
            candidates.push_back(modPath.parent_path() / "orca-hooks.bin");
            candidates.push_back(modPath.parent_path() / "src" / "orca-hooks.bin");
        }
    }
    {
        wchar_t exePath[MAX_PATH] = {0};
        DWORD len = GetModuleFileNameW(nullptr, exePath, MAX_PATH);
        if (len > 0) {
            fs::path exe(exePath);
            candidates.push_back(exe.parent_path() / "orca-hooks.bin");
            candidates.push_back(exe.parent_path() / "src" / "orca-hooks.bin");
        }
    }
#endif
    candidates.push_back(fs::path("cmake-build-relwithdebinfo-visual-studio-llvm/src/orca-hooks.bin"));
    candidates.push_back(fs::path("cmake-build-relwithdebinfo-visual-studio-llvm/generated/hook-sdkgen/runtime/orca-hooks.bin"));
    candidates.push_back(fs::path("src/orca-hooks.bin"));
    candidates.push_back(fs::path("../src/orca-hooks.bin"));
    candidates.push_back(fs::path("generated/hook-sdkgen/runtime/orca-hooks.bin"));
    for (auto &p : candidates) {
        std::error_code ec;
        if (fs::exists(p, ec) && !ec) {
            return p.string();
        }
    }
    return {};
}

void HookRuntime::SetManifestForTesting(std::unique_ptr<HookManifest> m){
    manifest_ = std::move(m);
    if (registry_) registry_ = std::make_unique<HookRegistry>(manifest_.get(), backend_.get());
    if (dispatcher_) dispatcher_ = std::make_unique<Dispatcher>(registry_.get());
}
void HookRuntime::SetBackendForTesting(std::unique_ptr<HookBackend> b){
    backend_ = std::move(b);
    active_build_ = backend_ ? backend_->ActiveBuildId() : BuildId{};
    if (registry_) registry_ = std::make_unique<HookRegistry>(manifest_.get(), backend_.get());
    if (dispatcher_) dispatcher_ = std::make_unique<Dispatcher>(registry_.get());
}

namespace {
thread_local PluginId t_abi_plugin_id{};

orca_hook_status_t host_get_build_id(orca_build_id_t* out) noexcept {
    if (!out || out->size < sizeof(orca_build_id_t)) return ORCA_HOOK_ERR_INVALID_SIZE;
    auto& rt = HookRuntime::Instance();
    if (!rt.IsInitialized()) return ORCA_HOOK_ERR_INTERNAL;
    BuildId bid = rt.ActiveBuildId();
    orca_build_id_t id = orca_hook::make_build_id();
    if (bid.os == 1) strncpy(id.os, "windows", ORCA_BUILD_ID_OS_MAX);
    else if (bid.os == 2) strncpy(id.os, "linux", ORCA_BUILD_ID_OS_MAX);
    else strncpy(id.os, "unknown", ORCA_BUILD_ID_OS_MAX);
    strncpy(id.arch, "x86_64", ORCA_BUILD_ID_ARCH_MAX);
    memcpy(id.image_sha256, bid.image_hash, 32);
    memcpy(id.pdb_guid, bid.debug_guid, 16);
    id.pdb_age = bid.debug_age;
    id.gnu_build_id_size = 0;
    memset(id.gnu_build_id, 0, 20);
    if (!bid.gnu_build_id.empty() && bid.gnu_build_id.size() <= 40) {
        size_t n = std::min<size_t>(bid.gnu_build_id.size()/2, 20);
        for (size_t i=0;i<n;++i){
            char hi = bid.gnu_build_id[i*2];
            char lo = bid.gnu_build_id[i*2+1];
            auto hex = [](char c)->uint8_t{ if(c>='0'&&c<='9') return c-'0'; if(c>='a'&&c<='f') return c-'a'+10; if(c>='A'&&c<='F') return c-'A'+10; return 0; };
            id.gnu_build_id[i] = (hex(hi)<<4)|hex(lo);
        }
        id.gnu_build_id_size = static_cast<uint32_t>(n);
    }
    *out = id;
    return ORCA_HOOK_OK;
}
orca_hook_status_t host_resolve_symbol(const char* symbol_id, void** out_address, uint64_t* out_rva) noexcept {
    if (!symbol_id || !out_address) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    auto* man = rt.Manifest();
    auto* be = rt.Backend();
    if (!man || man->Empty()) return ORCA_HOOK_ERR_NOT_FOUND;
    auto* sym = man->FindById(symbol_id);
    if (!sym) return ORCA_HOOK_ERR_NOT_FOUND;
    if (out_rva) *out_rva = sym->rva;
    if (be && be->ImageBase()) *out_address = reinterpret_cast<uint8_t*>(be->ImageBase()) + sym->rva;
    else *out_address = reinterpret_cast<void*>(sym->rva);
    return ORCA_HOOK_OK;
}
orca_hook_status_t host_resolve_rva(uint64_t rva, void** out_address) noexcept {
    if (!out_address) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    auto* be = rt.Backend();
    if (be && be->ImageBase()) *out_address = reinterpret_cast<uint8_t*>(be->ImageBase()) + rva;
    else *out_address = reinterpret_cast<void*>(rva);
    return ORCA_HOOK_OK;
}
orca_hook_status_t host_install_hook(const orca_hook_request_t* req, orca_hook_handle_t* out_handle) noexcept {
    if (!req || !out_handle) {
        HookRuntime::SetLastHookError("install_hook: null req or out_handle (INVALID_ARG)");
        return ORCA_HOOK_ERR_INVALID_ARG;
    }
    if (req->size < sizeof(orca_hook_request_t) || req->version != ORCA_HOOK_ABI_VERSION) {
        HookRuntime::SetLastHookError(std::string("install_hook hook '") + (req->hook_id ? req->hook_id : "<null>") + "': invalid size/version (INVALID_SIZE) req.size=" + std::to_string(req->size) + " version=" + std::to_string(req->version));
        return ORCA_HOOK_ERR_INVALID_SIZE;
    }
    if (!req->hook_id || !req->callback) {
        HookRuntime::SetLastHookError("install_hook: missing hook_id or callback (INVALID_ARG)");
        return ORCA_HOOK_ERR_INVALID_ARG;
    }
    auto& rt = HookRuntime::Instance();
    if (!rt.IsInitialized()) {
        HookRuntime::SetLastHookError("install_hook hook '" + std::string(req->hook_id) + "': runtime not initialized (INTERNAL)");
        return ORCA_HOOK_ERR_INTERNAL;
    }
    PluginId pid = t_abi_plugin_id;
    if (pid.empty()) pid = "abi_plugin";
    auto hr = std::make_shared<HookRequest>();
    hr->plugin_id = pid;
    hr->hook_id = req->hook_id ? req->hook_id : "";
    hr->target_id = req->target_symbol_id ? req->target_symbol_id : "";
    hr->target_rva = req->target_rva;
    hr->priority = req->priority ? static_cast<int32_t>(req->priority) : kDefaultPriority;
    switch (req->point){
        case ORCA_HOOK_POINT_ENTRY: hr->point = HookPoint::ENTRY; break;
        case ORCA_HOOK_POINT_RETURN: hr->point = HookPoint::RETURN; break;
        case ORCA_HOOK_POINT_INVOKE: hr->point = HookPoint::INVOKE; hr->invoke.ordinal = req->u.invoke.ordinal; break;
        case ORCA_HOOK_POINT_OFFSET: hr->point = HookPoint::OFFSET; hr->offset.rva = static_cast<uint32_t>(req->u.offset.rva); break;
        case ORCA_HOOK_POINT_VTABLE: hr->point = HookPoint::VTABLE; hr->vtable.slot = req->u.vtable.index; hr->vtable.per_instance = req->u.vtable.is_per_instance != 0; hr->vtable.instance = req->u.vtable.instance; break;
        case ORCA_HOOK_POINT_IAT: hr->point = HookPoint::IAT; hr->import.module = req->u.import_.module ? req->u.import_.module : ""; hr->import.symbol = req->u.import_.name ? req->u.import_.name : ""; break;
        case ORCA_HOOK_POINT_GOT: hr->point = HookPoint::GOT; hr->import.module = req->u.import_.module ? req->u.import_.module : ""; hr->import.symbol = req->u.import_.name ? req->u.import_.name : ""; break;
        default: {
            std::string msg = "install_hook hook '" + hr->hook_id + "' target '" + hr->target_id + "': invalid point " + std::to_string(req->point) + " (INVALID_ARG)";
            HookRuntime::SetLastHookError(msg);
            return ORCA_HOOK_ERR_INVALID_ARG;
        }
    }
    switch (req->kind){
        case ORCA_HOOK_KIND_BEFORE: hr->kind = HookKind::Before; break;
        case ORCA_HOOK_KIND_AFTER: hr->kind = HookKind::After; break;
        case ORCA_HOOK_KIND_REPLACE: hr->kind = HookKind::Replace; break;
        default: {
            std::string msg = "install_hook hook '" + hr->hook_id + "' target '" + hr->target_id + "': invalid kind " + std::to_string(req->kind) + " (INVALID_ARG)";
            HookRuntime::SetLastHookError(msg);
            return ORCA_HOOK_ERR_INVALID_ARG;
        }
    }
    hr->abi_callback = req->callback;
    hr->abi_user_data = req->user_data;
    std::string err;
    std::vector<std::shared_ptr<HookRequest>> v{hr};
    if (!rt.Registry()->InstallTransaction(pid, v, err)){
        orca_hook_status_t st = ORCA_HOOK_ERR_PATCH_FAILED;
        std::string low = err;
        for (auto &c : low) c = static_cast<char>(::tolower(c));
        if (low.find("instruction boundary") != std::string::npos) st = ORCA_HOOK_ERR_BAD_INSTRUCTION_BOUNDARY;
        else if (low.find("outside symbol ranges") != std::string::npos || low.find("bad_rva") != std::string::npos || low.find("bad rva") != std::string::npos) st = ORCA_HOOK_ERR_BAD_RVA;
        else if (low.find("offset out of range") != std::string::npos) st = ORCA_HOOK_ERR_BAD_RVA;
        else if (low.find("target_id not found") != std::string::npos || low.find("not found") != std::string::npos) {
            if (low.find("iat") != std::string::npos || low.find("import") != std::string::npos) st = ORCA_HOOK_ERR_IMPORT_NOT_FOUND;
            else st = ORCA_HOOK_ERR_NOT_FOUND;
        }
        else if (low.find("duplicate hook_id") != std::string::npos) st = ORCA_HOOK_ERR_ALREADY_EXISTS;
        else if (low.find("already exists") != std::string::npos) st = ORCA_HOOK_ERR_ALREADY_EXISTS;
        else if (low.find("too many mid hooks") != std::string::npos) st = ORCA_HOOK_ERR_BUSY;
        else if (low.find("build_id mismatch") != std::string::npos || low.find("build mismatch") != std::string::npos) st = ORCA_HOOK_ERR_BUILD_MISMATCH;
        else if (low.find("safetyhook") != std::string::npos) st = ORCA_HOOK_ERR_PATCH_FAILED;
        else if (low.find("protect") != std::string::npos) st = ORCA_HOOK_ERR_PROTECT_FAILED;
        else if (low.find("vtable") != std::string::npos && low.find("bounds") != std::string::npos) st = ORCA_HOOK_ERR_VTABLE_BOUNDS;
        else if (low.find("invalid") != std::string::npos) st = ORCA_HOOK_ERR_INVALID_ARG;
        else if (low.find("resolve") != std::string::npos) st = ORCA_HOOK_ERR_RESOLVE_FAILED;
        std::string detailed = "hook '" + hr->hook_id + "' target '" + (hr->target_id.empty() ? ("rva=0x" + [] (uint64_t v){ char buf[32]; snprintf(buf,sizeof(buf),"%llx",(unsigned long long)v); return std::string(buf); }(hr->target_rva)) : hr->target_id) + "' point=" + std::string(Slic3r::Hook::to_string(hr->point)) + " kind=" + std::string(Slic3r::Hook::to_string(hr->kind)) + " priority=" + std::to_string(hr->priority) + " failed: " + err + " (status " + std::to_string(st) + " " + HookRuntime::HookStatusName(st) + ")";
        HookRuntime::SetLastHookError(detailed);
        return st;
    }
    HookRuntime::SetLastHookError({});
    *out_handle = hr->backend_handle ? hr->backend_handle : reinterpret_cast<orca_hook_handle_t>(hr.get());
    return ORCA_HOOK_OK;
}
orca_hook_status_t host_remove_hook(orca_hook_handle_t handle) noexcept {
    if (!handle) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    auto* reg = rt.Registry();
    auto* be = rt.Backend();
    if (!reg) return ORCA_HOOK_ERR_INTERNAL;
    // Resolve owning chain under the registry lock, then unpatch outside it.
    // A RETURN chain owns one trap per RET site; remove them together.
    std::vector<HookHandle> handles;
    if (reg->FindBackendHandles(handle, handles)) {
        if (be && be->IsSupported()) {
            for (auto h : handles) {
                std::string e;
                be->Remove(h, e);
            }
        }
        return ORCA_HOOK_OK;
    }
    if (be){
        std::string e;
        if (be->Remove(handle, e)) return ORCA_HOOK_OK;
    }
    return ORCA_HOOK_ERR_NOT_FOUND;
}
orca_hook_status_t host_call_next(orca_cpu_context_t* ctx) noexcept {
    if (!ctx) return ORCA_HOOK_ERR_INVALID_ARG;
    return ORCA_HOOK_ERR_INTERNAL;
}
orca_hook_status_t host_call_original(orca_cpu_context_t* ctx) noexcept {
    if (!ctx) return ORCA_HOOK_ERR_INVALID_ARG;
    return ORCA_HOOK_ERR_INTERNAL;
}
orca_hook_status_t host_read_memory(const void* src, void* dst, size_t size) noexcept {
    if (!src || !dst || !size) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    return rt.ReadMemory(src,dst,size) ? ORCA_HOOK_OK : ORCA_HOOK_ERR_INTERNAL;
}
orca_hook_status_t host_write_memory(void* dst, const void* src, size_t size) noexcept {
    if (!dst || !src || !size) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    return rt.WriteMemory(dst,src,size) ? ORCA_HOOK_OK : ORCA_HOOK_ERR_INTERNAL;
}
orca_hook_status_t host_protect_memory(void* addr, size_t size, uint32_t new_prot, uint32_t* out_old) noexcept {
    if (!addr || !size) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    uint32_t p=0;
    if (new_prot & ORCA_PROTECT_READ) p |= static_cast<uint32_t>(MemProt::Read);
    if (new_prot & ORCA_PROTECT_WRITE) p |= static_cast<uint32_t>(MemProt::Write);
    if (new_prot & ORCA_PROTECT_EXECUTE) p |= static_cast<uint32_t>(MemProt::Execute);
    MemProt mp = static_cast<MemProt>(p);
    MemProt old{};
    bool ok = rt.ProtectMemory(addr,size,mp,out_old ? &old : nullptr);
    if (out_old){
        uint32_t o=0;
        if ((static_cast<uint32_t>(old) & static_cast<uint32_t>(MemProt::Read)) !=0) o|=ORCA_PROTECT_READ;
        if ((static_cast<uint32_t>(old) & static_cast<uint32_t>(MemProt::Write)) !=0) o|=ORCA_PROTECT_WRITE;
        if ((static_cast<uint32_t>(old) & static_cast<uint32_t>(MemProt::Execute)) !=0) o|=ORCA_PROTECT_EXECUTE;
        *out_old = o;
    }
    return ok ? ORCA_HOOK_OK : ORCA_HOOK_ERR_PROTECT_FAILED;
}
orca_hook_status_t host_flush_icache(void* addr, size_t size) noexcept {
    if (!addr || !size) return ORCA_HOOK_ERR_INVALID_ARG;
    auto& rt = HookRuntime::Instance();
    rt.FlushICache(addr,size);
    return ORCA_HOOK_OK;
}
void host_log(int32_t level, const char* msg) noexcept {
    (void)level; (void)msg;
}
orca_hook_status_t host_set_error(const char* msg) noexcept {
    (void)msg; return ORCA_HOOK_OK;
}
const char* host_get_data_dir() noexcept {
    try {
        static std::string cached;
        const std::string& s = Slic3r::data_dir();
        cached = s;
        return cached.c_str();
    } catch (...) {
        return nullptr;
    }
}
} // namespace

const orca_host_api_v1_t* HookRuntime::GetHostApi() const noexcept {
    static orca_host_api_v1_t table{};
    static bool inited=false;
    if (!inited){
        table = orca_hook::make_host_api();
        table.get_build_id = &host_get_build_id;
        table.resolve_symbol = &host_resolve_symbol;
        table.resolve_rva = &host_resolve_rva;
        table.install_hook = &host_install_hook;
        table.remove_hook = &host_remove_hook;
        table.call_next = &host_call_next;
        table.call_original = &host_call_original;
        table.read_memory = &host_read_memory;
        table.write_memory = &host_write_memory;
        table.protect_memory = &host_protect_memory;
        table.flush_icache = &host_flush_icache;
        table.log = &host_log;
        table.set_error = &host_set_error;
        table.get_data_dir = &host_get_data_dir;
        inited=true;
    }
    return &table;
}
void HookRuntime::SetCurrentPluginForAbi(const PluginId& id){ t_abi_plugin_id = id; }
PluginId HookRuntime::GetCurrentPluginForAbi(){ return t_abi_plugin_id; }

std::unique_ptr<HookBackend> HookBackend::CreateForCurrentPlatform(){
#ifdef _WIN32
    return std::make_unique<WindowsHookBackend>();
#else
    return std::make_unique<NullHookBackend>();
#endif
}
std::unique_ptr<HookBackend> HookBackend::CreateNull(){
    return std::make_unique<NullHookBackend>();
}

} // namespace Slic3r::Hook
