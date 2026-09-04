#pragma once

#include "HookBackend.hpp"

#ifdef _WIN32
#include <windows.h>
#endif

namespace Slic3r::Hook {

// Windows x64 backend: SafetyHook inline hooks, int3 traps, manual IAT and
// page-protection helpers. Compiled even on non-Windows (IsSupported=false)
// to keep the interface uniform. The backend patches a single target;
// transactionality lives in the registry.

class WindowsHookBackend final : public HookBackend {
public:
    WindowsHookBackend();
    ~WindowsHookBackend() override;

    bool IsSupported() const noexcept override;
    std::string Name() const override { return "windows_x64_safetyhook"; }
    void* ImageBase() const noexcept override;
    BuildId ActiveBuildId() const override;

    InstallResult InstallInline(const TargetInfo& target, void* detour) override;
    InstallResult InstallVTable(const TargetInfo& target, void* detour) override;
    InstallResult InstallIAT(const TargetInfo& target, void* detour) override;
    bool FindReturnOffsets(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) override;
    bool FindInstructionBoundaries(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) override;
    InstallResult InstallTrap(const TargetInfo& target, uint32_t offset_within_symbol) override;

    bool Remove(HookHandle handle, std::string& error) override;

    bool ReadMemory(const void* src, void* dst, size_t sz) noexcept override { return HookMemory::Read(src,dst,sz); }
    bool WriteMemory(void* dst, const void* src, size_t sz) noexcept override { return HookMemory::Write(dst,src,sz); }
    bool ProtectMemory(void* addr, size_t sz, MemProt np, MemProt* old) noexcept override { return HookMemory::Protect(addr,sz,np,old); }
    void FlushICache(void* addr, size_t sz) noexcept override { HookMemory::FlushICache(addr,sz); }

private:
    struct Impl;
    Impl* impl_ = nullptr;
    void* image_base_ = nullptr;
    BuildId active_build_{};
    bool supported_ = false;

    void InitBuildId();
    void* RvaToVa(uint64_t rva) const noexcept;
    // Pristine function bytes from the image FILE on disk (PE section map),
    // never live memory: live code may already carry this process's own
    // patches, whose bytes desynchronize the decode into spurious sites.
    bool ReadPristine(uint64_t rva, uint32_t size, std::vector<uint8_t>& out) const;
    InstallResult MakeError(const std::string& msg) const;
};

} // namespace Slic3r::Hook
