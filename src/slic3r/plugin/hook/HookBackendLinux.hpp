#pragma once
#include "HookBackend.hpp"

#ifdef __linux__
#include <link.h>
#endif

namespace Slic3r::Hook {

// Linux x64 backend: SafetyHook inline hooks, int3 traps, manual GOT/PLT and
// page-protection helpers plus SysV stubs. No macOS support.
// Mirrors WindowsHookBackend API but uses ELF/GOT semantics and SysV

class LinuxHookBackend final : public HookBackend {
public:
    LinuxHookBackend();
    ~LinuxHookBackend() override;

    bool IsSupported() const noexcept override;
    std::string Name() const override { return "linux_x64_safetyhook"; }
    void* ImageBase() const noexcept override;
    BuildId ActiveBuildId() const override;

    InstallResult InstallInline(const TargetInfo& target, void* detour) override;
    InstallResult InstallVTable(const TargetInfo& target, void* detour) override;
    InstallResult InstallIAT(const TargetInfo& target, void* detour) override; // GOT/PLT
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
    // Pristine function bytes from the image FILE on disk (ELF PT_LOAD map),
    // never live memory; same rationale as the Windows backend.
    bool ReadPristine(uint64_t rva, uint32_t size, std::vector<uint8_t>& out) const;
    InstallResult MakeError(const std::string& msg) const;
    void* FindGotSlot(const std::string& module, const std::string& symbol, std::string& err) const;
    InstallResult InstallGot(const TargetInfo& target, void* detour);
};

} // namespace Slic3r::Hook
