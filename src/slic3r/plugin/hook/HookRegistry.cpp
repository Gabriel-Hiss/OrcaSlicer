#include "HookRegistry.hpp"
#include "CpuContext.hpp"
#include <algorithm>
#include <cctype>
#include <chrono>
#include <cstdio>

extern "C" void orca_hook_inline_stub() noexcept;

namespace Slic3r::Hook {

bool HookRegistry::ValidateOffsetBoundary(const ManifestSymbol& sym, uint64_t abs_rva, std::string& error) const {
    if (manifest_ && manifest_->IsValidInstructionBoundary(sym, abs_rva))
        return true;
    if (sym.instr_offsets.empty() && backend_ && backend_->IsSupported() && sym.size != 0 &&
        abs_rva >= sym.rva && abs_rva < sym.rva + sym.size) {
        // No instruction map in the slim manifest: decode pristine bytes for the boundary check.
        std::vector<uint32_t> bounds;
        if (backend_->FindInstructionBoundaries(sym.rva, sym.size, bounds) && !bounds.empty()) {
            uint32_t want = static_cast<uint32_t>(abs_rva - sym.rva);
            if (std::binary_search(bounds.begin(), bounds.end(), want))
                return true;
            uint32_t near = FindNearestInstructionBoundary(bounds, want);
            char req_hex[32], near_hex[32];
            snprintf(req_hex, sizeof(req_hex), "0x%llX", static_cast<unsigned long long>(abs_rva));
            snprintf(near_hex, sizeof(near_hex), "0x%llX", static_cast<unsigned long long>(sym.rva + near));
            error = std::string("OFFSET ") + req_hex + " is not at a valid instruction boundary of '" +
                    sym.id + "'; nearest valid boundary is " + near_hex + " (instruction boundary)";
            return false;
        }
    }
    error = "OFFSET not at validated instruction boundary";
    return false;
}

bool HookRegistry::ResolveTarget(const HookRequest& req, TargetInfo& out, std::string& error) const {
    out = TargetInfo{};
    out.point = req.point;
    out.import_module = req.import.module;
    out.import_symbol = req.import.symbol;
    out.vtable_slot = req.vtable.slot;
    out.vtable_per_instance = req.vtable.per_instance;
    out.vtable_instance = req.vtable.instance;
    out.invoke_ordinal = req.invoke.ordinal;
    out.offset_rva = req.offset.rva;

    if (req.point == HookPoint::IAT || req.point == HookPoint::GOT) {
        out.id = req.target_id;
        out.rva = req.target_rva;
        if (req.import.symbol.empty()) { error="IAT requires import_symbol"; return false; }
        return true;
    }
    if (req.point == HookPoint::VTABLE) {
        if (!req.target_id.empty() && manifest_) {
            auto* sym = manifest_->FindById(req.target_id);
            if (!sym) { error="vtable target_id not found: "+req.target_id; return false; }
            out.id = req.target_id;
            out.rva = sym->rva;
            out.absolute = reinterpret_cast<void*>(reinterpret_cast<uintptr_t>(backend_ ? backend_->ImageBase():nullptr)+sym->rva);
            out.size = sym->size;
        } else if (req.vtable.instance || req.target_rva) {
            out.rva = req.target_rva;
            out.absolute = req.vtable.instance ? *reinterpret_cast<void**>(req.vtable.instance) : nullptr;
            if (req.vtable.per_instance && req.vtable.instance) {
                out.absolute = *reinterpret_cast<void**>(req.vtable.instance);
            }
        } else {
            error="vtable requires target_id or instance";
            return false;
        }
        return true;
    }

    std::string target_id = req.target_id;
    uint64_t rva = req.target_rva;
    const ManifestSymbol* sym = nullptr;
    if (!target_id.empty() && manifest_) {
        sym = manifest_->FindById(target_id);
        if (!sym) { error="target_id not found: "+target_id; return false; }
        out.id = target_id;
        out.rva = sym->rva;
        out.size = sym->size;
        if (req.point == HookPoint::OFFSET) {
            uint64_t abs_rva = req.offset.rva;
            if (abs_rva == 0) { error="OFFSET requires offset.rva"; return false; }
            bool inside = (abs_rva >= sym->rva && abs_rva < sym->rva + sym->size);
            if (!inside) {
                bool inRange=false;
                for (auto& rg: sym->ranges) if (abs_rva >= rg.rva && abs_rva < rg.rva + rg.size) inRange=true;
                if (!inRange){ error="OFFSET RVA outside symbol ranges"; return false; }
            }
            if (!ValidateOffsetBoundary(*sym, abs_rva, error))
                return false;
            out.rva = abs_rva;
            out.offset_rva = static_cast<uint32_t>(abs_rva);
        } else if (req.point == HookPoint::INVOKE) {
            out.invoke_ordinal = req.invoke.ordinal;
        }
        if (backend_) {
            out.absolute = reinterpret_cast<uint8_t*>(backend_->ImageBase()) + out.rva;
        }
        return true;
    } else if (rva != 0) {
        out.rva = rva;
        if (req.point == HookPoint::OFFSET && manifest_) {
            auto* byRva = manifest_->FindByRva(rva);
            if (byRva && !ValidateOffsetBoundary(*byRva, rva, error))
                return false;
        }
        if (backend_) out.absolute = reinterpret_cast<uint8_t*>(backend_->ImageBase()) + rva;
        return true;
    }
    error="target_id or target_rva required";
    return false;
}

std::string HookRegistry::ChainKey(const TargetInfo& t) const {
    std::string k = t.id + "|" + std::to_string(static_cast<int>(t.point));
    if (t.point == HookPoint::OFFSET) k += "|off:" + std::to_string(t.offset_rva);
    else if (t.point == HookPoint::VTABLE) k += "|vt:" + std::to_string(t.vtable_slot) + (t.vtable_per_instance ? ":inst" : ":glob");
    else if (t.point == HookPoint::INVOKE) k += "|inv:" + std::to_string(t.invoke_ordinal);
    else if (t.point == HookPoint::IAT || t.point == HookPoint::GOT) k += "|iat:" + t.import_module + "!" + t.import_symbol;
    if (t.id.empty() && t.rva) k += "|rva:" + std::to_string(t.rva);
    return k;
}

void HookRegistry::SortChain(TargetChain& chain) {
    std::sort(chain.hooks.begin(), chain.hooks.end(), [](const auto& a, const auto& b){
        if (a->priority != b->priority) return a->priority > b->priority;
        if (a->plugin_id != b->plugin_id) return a->plugin_id < b->plugin_id;
        return a->declare_order < b->declare_order;
    });
}

bool HookRegistry::InstallOneLocked(const std::shared_ptr<HookRequest>& req, std::string& error) {
    TargetInfo target;
    if (!ResolveTarget(*req, target, error)) return false;

    std::string key = ChainKey(target);
    auto it = chains_.find(key);
    std::shared_ptr<TargetChain> chain;
    if (it == chains_.end()){
        chain = std::make_shared<TargetChain>();
        chain->target = target;
        chains_[key]=chain;
    } else {
        chain = it->second;
        for (auto& h: chain->hooks) if (h->hook_id == req->hook_id){ error="duplicate hook_id "+req->hook_id; return false; }
    }

    chain->hooks.push_back(req);
    SortChain(*chain);
    // Backend patching happens in InstallTransaction phase 2; order only is tracked here.
    return true;
}

bool HookRegistry::InstallOne(const std::shared_ptr<HookRequest>& req, std::string& error) {
    std::lock_guard<std::mutex> g(mutex_);
    return InstallOneLocked(req, error);
}

bool HookRegistry::InstallReturnSites(const std::shared_ptr<TargetChain>& chain,
                                      const std::shared_ptr<HookRequest>& req,
                                      const TargetInfo& target,
                                      std::vector<HookHandle>& site_handles_out,
                                      std::string& error) {
    if (!chain || !req) { error = "null chain or request for RETURN install"; return false; }
    if (target.size == 0) { error = "RETURN requires a manifest symbol with known size"; return false; }
    if (!backend_ || !backend_->IsSupported()) { error = "backend not supported on this platform"; return false; }
    std::vector<uint32_t> ret_offsets;
    if (!backend_->FindReturnOffsets(target.rva, target.size, ret_offsets) || ret_offsets.empty()) {
        error = "RETURN has no instrumentable return sites for target '" + target.id + "'";
        return false;
    }
    // The stored TargetInfo keeps the symbol RVA so every site resolves to the
    // same chain key; only the install offset varies per site.
    // Sites are breakpoint-traps (int3, 1 byte), not SafetyHook mid-hooks: a
    // trap relocates nothing, so it fires regardless of ENTRY patches on the
    // same function and cannot desynchronize like a stolen prologue can.
    std::vector<HookHandle> installed;
    for (uint32_t off : ret_offsets) {
        if (off >= target.size) {
            error = "RETURN site offset out of range for target '" + target.id + "'";
            break;
        }
        InstallResult res = backend_->InstallTrap(target, off);
        if (!res.ok) {
            error = res.error;
            break;
        }
        installed.push_back(res.handle);
    }
    if (installed.size() != ret_offsets.size()) {
        for (auto h : installed) {
            std::string e;
            backend_->Remove(h, e);
        }
        return false;
    }
    {
        std::lock_guard<std::mutex> g(mutex_);
        chain->backend_install_handle = installed.front();
        chain->extra_backend_handles.assign(installed.begin() + 1, installed.end());
        chain->trampoline = nullptr;
        req->backend_handle = installed.front();
        req->trampoline = nullptr;
    }
    site_handles_out.insert(site_handles_out.end(), installed.begin() + 1, installed.end());
    return true;
}

bool HookRegistry::InstallTransaction(const PluginId& plugin_id,
                                       std::vector<std::shared_ptr<HookRequest>> requests,
                                       std::string& error) {

    {
        std::lock_guard<std::mutex> g(mutex_);
        for (auto& r: requests){
            TargetInfo ti;
            if (!ResolveTarget(*r, ti, error)) return false;
        }
        std::sort(requests.begin(), requests.end(), [](const auto& a, const auto& b){
            if (a->priority != b->priority) return a->priority > b->priority;
            if (a->plugin_id != b->plugin_id) return a->plugin_id < b->plugin_id;
            return a->declare_order < b->declare_order;
        });
        for (auto& r: requests){
            if (!InstallOneLocked(r, error)){
                for (auto& inserted : requests){
                    if (inserted == r) break;
                    UnlinkHookLocked(inserted->hook_id);
                }
                return false;
            }
        }
    }
    std::vector<std::shared_ptr<HookRequest>> installed_backend;
    std::vector<HookHandle> installed_site_handles;
    auto remove_handle = [&](HookHandle h){
        if (h && backend_ && backend_->IsSupported()){
            std::string e;
            backend_->Remove(h, e);
        }
    };
    auto do_rollback = [&](){
        std::lock_guard<std::mutex> g(mutex_);
        for (auto& br : installed_backend){
            remove_handle(br->backend_handle);
            UnlinkHookLocked(br->hook_id);
        }
        for (auto h : installed_site_handles) remove_handle(h);
        for (auto& pending : requests){
            bool was_installed = false;
            for (auto& ib : installed_backend) if (ib->hook_id==pending->hook_id) was_installed=true;
            if (!was_installed) UnlinkHookLocked(pending->hook_id);
        }
    };
    for (auto& r : requests){
        std::shared_ptr<TargetChain> chain;
        {
            std::lock_guard<std::mutex> g(mutex_);
            TargetInfo ti;
            std::string e;
            ResolveTarget(*r, ti, e);
            auto key = ChainKey(ti);
            auto it = chains_.find(key);
            if (it==chains_.end()) { error="chain lost after insert"; do_rollback(); return false; }
            chain = it->second;
            if (chain->backend_install_handle) {
                r->backend_handle = chain->backend_install_handle;
                r->trampoline = chain->trampoline;
                continue;
            }
        }
        if (!backend_ || !backend_->IsSupported()){
            error = "backend not supported on this platform";
            do_rollback();
            return false;
        }
        TargetInfo ti2;
        if (!ResolveTarget(*r, ti2, error)) { do_rollback(); return false; }
        if (ti2.point == HookPoint::RETURN) {
            if (!InstallReturnSites(chain, r, ti2, installed_site_handles, error)) { do_rollback(); return false; }
            installed_backend.push_back(r);
            continue;
        }
        void* detour = nullptr;
        switch (ti2.point){
            case HookPoint::VTABLE:
            case HookPoint::IAT:
            case HookPoint::GOT:
                detour = reinterpret_cast<void*>(&orca_hook_inline_stub);
                break;
            default: break;
        }
        InstallResult res{};
        switch (ti2.point){
            case HookPoint::ENTRY:
                res = backend_->InstallTrap(ti2, 0);
                break;
            case HookPoint::OFFSET:
                res = backend_->InstallTrap(ti2, static_cast<uint32_t>(ti2.offset_rva - ti2.rva));
                break;
            case HookPoint::INVOKE:
                res = backend_->InstallTrap(ti2, static_cast<uint32_t>(ti2.offset_rva - ti2.rva));
                break;
            case HookPoint::VTABLE:
                res = backend_->InstallVTable(ti2, detour);
                break;
            case HookPoint::IAT:
            case HookPoint::GOT:
                res = backend_->InstallIAT(ti2, detour);
                break;
            default: res.ok=false; res.error="unknown point";
        }
        if (!res.ok){
            error = res.error;
            do_rollback();
            return false;
        }
        {
            std::lock_guard<std::mutex> g(mutex_);
            chain->backend_install_handle = res.handle;
            chain->trampoline = res.trampoline;
            r->backend_handle = res.handle;
            r->trampoline = res.trampoline;
        }
        installed_backend.push_back(r);
    }

    return true;
}
void HookRegistry::UnlinkHookLocked(const HookId& hid) {
    for (auto it = chains_.begin(); it != chains_.end(); ) {
        auto& chain = it->second;
        auto& vec = chain->hooks;
        auto origSize = vec.size();
        vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto& h){ return h->hook_id==hid; }), vec.end());
        if (vec.empty()){
            // Chain empty: schedule backend removal after drain (caller will do)
            it = chains_.erase(it);
        } else {
            if (vec.size()!=origSize) SortChain(*chain);
            ++it;
        }
    }
    hook_active_.erase(hid);
}

HookStatus HookRegistry::RemovePluginHooks(const PluginId& plugin_id, uint32_t drain_timeout_ms, std::string& error) {
    std::vector<std::shared_ptr<TargetChain>> affected;
    std::vector<HookId> hook_ids;
    {
        std::lock_guard<std::mutex> g(mutex_);
        // Collect EVERY hook of the plugin: stopping at the first match per
        // chain used to orphan the remaining hooks of a shared chain, leaving
        // the chain registered and its backend patch installed forever.
        for (auto& kv : chains_){
            bool touched = false;
            for (auto& h : kv.second->hooks) if (h->plugin_id==plugin_id){
                hook_ids.push_back(h->hook_id);
                touched = true;
            }
            if (touched) affected.push_back(kv.second);
        }
        // Unlink first to prevent new entries.
        for (auto& hid : hook_ids) {
            for (auto& ch : affected) {
                auto& vec = ch->hooks;
                vec.erase(std::remove_if(vec.begin(), vec.end(), [&](const auto& h){ return h->hook_id==hid; }), vec.end());
            }
            hook_active_.erase(hid);
        }
        // Empty chains leave the map but keep the backend handle for removal after drain.
        for (auto it = chains_.begin(); it != chains_.end(); ) {
            bool contains = false;
            for (auto& a : affected) if (a.get()==it->second.get()) contains=true;
            if (contains && it->second->hooks.empty()){
                it = chains_.erase(it);
            } else {
                for (auto& a : affected) if (a.get()==it->second.get()) SortChain(*it->second);
                ++it;
            }
        }
    }

    auto deadline = std::chrono::steady_clock::now() + std::chrono::milliseconds(drain_timeout_ms);
    bool timed_out = false;
    for (auto& ch : affected){
        while (ch->active_calls.load() != 0){
            if (std::chrono::steady_clock::now() >= deadline){ timed_out=true; break; }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }
        if (timed_out) break;
    }
    if (timed_out){
        error = "drain timeout - restart_required";
        // Timed out: leave backend patches installed; the code may still be in use.
        return HookStatus::RestartRequired;
    }

    for (auto& ch : affected){
        if (ch->hooks.empty() && ch->backend_install_handle){
            if (backend_ && backend_->IsSupported()){
                // A RETURN chain owns one trap per RET site; remove them all.
                std::vector<HookHandle> handles{ch->backend_install_handle};
                handles.insert(handles.end(), ch->extra_backend_handles.begin(), ch->extra_backend_handles.end());
                for (auto h : handles) {
                    std::string re;
                    if (backend_->Remove(h, re)) continue;
                    // A missing handle only means another owner already removed
                    // the shared patch (e.g. host remove_hook during unload).
                    std::string low = re;
                    for (auto& c : low) c = static_cast<char>(::tolower(static_cast<unsigned char>(c)));
                    if (low.find("not found") != std::string::npos) continue;
                    error = re;
                    return HookStatus::RestartRequired;
                }
            }
            ch->backend_install_handle = nullptr;
            ch->extra_backend_handles.clear();
        }
    }

    plugin_active_.erase(plugin_id);
    return HookStatus::Ok;
}

void HookRegistry::DisableHookForSession(const HookId& hook_id) {
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& kv: chains_){
        for (auto& h: kv.second->hooks) if (h->hook_id==hook_id){
            h->disabled_this_session = true;
        }
    }
}

std::shared_ptr<TargetChain> HookRegistry::FindChain(const TargetId& target_id, uint64_t rva, HookPoint point) const {
    std::lock_guard<std::mutex> g(mutex_);
    TargetInfo ti; ti.id=target_id; ti.rva=rva; ti.point=point;
    auto key = ChainKey(ti);
    auto it=chains_.find(key);
    if(it!=chains_.end()) return it->second;
    for (auto& kv: chains_){
        if (kv.second->target.rva==rva && kv.second->target.point==point) return kv.second;
    }
    return nullptr;
}

void HookRegistry::SnapshotHooks(const std::shared_ptr<TargetChain>& chain,
                                 std::vector<std::shared_ptr<HookRequest>>& out) const {
    std::lock_guard<std::mutex> g(mutex_);
    if (chain) out = chain->hooks;
    else out.clear();
}

bool HookRegistry::FindBackendHandles(HookHandle handle, std::vector<HookHandle>& out) const {
    out.clear();
    if (!handle) return false;
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& kv : chains_) {
        auto& ch = kv.second;
        if (ch->backend_install_handle != handle &&
            std::find(ch->extra_backend_handles.begin(), ch->extra_backend_handles.end(), handle) == ch->extra_backend_handles.end())
            continue;
        if (ch->backend_install_handle) out.push_back(ch->backend_install_handle);
        out.insert(out.end(), ch->extra_backend_handles.begin(), ch->extra_backend_handles.end());
        return !out.empty();
    }
    return false;
}

std::vector<std::shared_ptr<TargetChain>> HookRegistry::AllChains() const {
    std::lock_guard<std::mutex> g(mutex_);
    std::vector<std::shared_ptr<TargetChain>> out;
    out.reserve(chains_.size());
    for (auto& kv: chains_) out.push_back(kv.second);
    return out;
}

void HookRegistry::ResetSessionDisables(const PluginId& plugin_id){
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& kv: chains_)
        for (auto& h: kv.second->hooks)
            if (h->plugin_id==plugin_id) { h->disabled_this_session=false; h->last_error.clear(); }
}
void HookRegistry::ResetAllSessionDisables(){
    std::lock_guard<std::mutex> g(mutex_);
    for (auto& kv: chains_)
        for (auto& h: kv.second->hooks) { h->disabled_this_session=false; h->last_error.clear(); }
}

uint32_t HookRegistry::ActiveCallsForPlugin(const PluginId& pid) const {
    auto it=plugin_active_.find(pid);
    if(it==plugin_active_.end()) return 0;
    return it->second.load();
}
uint32_t HookRegistry::ActiveCallsForHook(const HookId& hid) const {
    auto it=hook_active_.find(hid);
    if(it==hook_active_.end()) return 0;
    return it->second.load();
}

} // namespace Slic3r::Hook
