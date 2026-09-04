#pragma once

#include "CpuContext.hpp"
#include "HookDefs.hpp"
#include "HookManifest.hpp"
#include "HookMemory.hpp"
#include <cstdint>
#include <functional>
#include <memory>
#include <string>
#include <vector>
namespace Slic3r::Hook {

// Decodes executable bytes and collects the start offset (relative to bytes)
// of EVERY instruction using a real decoder (Zydis when available), reusing
// the same decode infrastructure as ScanReturnOffsets below. Returns false
// when no decoder is available or the bytes fail to decode.
bool ScanInstructionBoundaries(const uint8_t* bytes, size_t size, std::vector<uint32_t>& out);
// Nearest valid boundary to `want` (exact hit returns `want`; ties prefer the
// lower boundary). Used to name the valid alternative in boundary errors.
// `bounds` must be sorted ascending, as produced by ScanInstructionBoundaries.
uint32_t FindNearestInstructionBoundary(const std::vector<uint32_t>& bounds, uint32_t want);
// Decodes the single instruction at code[0..len) and, if it is a RET,
// computes the resume state exactly as the CPU would: pop the return address
// from the stacked slot. stacked_retaddr is the qword at [rsp]. Handles
// C3/CB (pop 8) and C2/CA with imm16 (pop 8+imm). Returns false for
// non-RET or undecodable bytes. Pure function (unit-testable, no OS).
bool EmulateReturnResume(const uint8_t* code, size_t len, uint64_t stacked_retaddr,
                         uint64_t rsp, uint64_t& out_rip, uint64_t& out_rsp);

// Decodes executable bytes and collects the offsets (relative to bytes) of
// RET instructions using a real decoder (Zydis when available), so opcode
// bytes hiding inside other instructions (e.g. EB C3 is JMP, not RET) are
// never mistaken for return sites. Returns false when no decoder is
// available, the bytes fail to decode, or no RET is found.
bool ScanReturnOffsets(const uint8_t* bytes, size_t size, std::vector<uint32_t>& out);


// Abstract backend interface, OS-portable and language-neutral.
// In-function points use int3 traps; VTABLE and IAT/GOT use direct slot swaps.
// SafetyHook inline hooks remain for detours that need a trampoline.

struct TargetInfo {
    TargetId id;       // manifest symbol id (empty if RVA-only OFFSET)
    uint64_t rva = 0;  // RVA from image base (validated)
    void* absolute = nullptr; // resolved VA (base + rva) or direct address
    HookPoint point = HookPoint::ENTRY;
    // Point-specific details filled by registry after manifest lookup
    uint32_t offset_rva = 0;      // for OFFSET
    uint32_t vtable_slot = 0;     // for VTABLE
    bool vtable_per_instance = false;
    void* vtable_instance = nullptr;
    uint32_t invoke_ordinal = 0;  // for INVOKE
    std::string import_module;    // for IAT/GOT
    std::string import_symbol;
    uint32_t size = 0;            // symbol size for validation
};

struct InstallResult {
    bool ok = false;
    std::string error;
    HookHandle handle = nullptr;   // backend handle for removal
    void* trampoline = nullptr;    // for inline hooks: original callable
    void* original_import = nullptr; // for IAT: previous pointer
};

class HookBackend {
public:
    virtual ~HookBackend() = default;
    // False where patching is unavailable; diagnostics hide hook UI then.
    virtual bool IsSupported() const noexcept = 0;
    virtual std::string Name() const = 0;
    // Resolve image base for RVA translation. Implemented via GetModuleHandle / dladdr.
    virtual void* ImageBase() const noexcept = 0;
    virtual BuildId ActiveBuildId() const = 0;
    // Core primitives:
    // ENTRY/OFFSET/INVOKE -> breakpoint-trap (int3, 1 byte, no relocation)
    // RETURN -> one breakpoint-trap per RET site sharing the chain
    // VTABLE -> slot patch (global) or per-instance clone
    // IAT/GOT -> import pointer swap
    virtual InstallResult InstallInline(const TargetInfo& target, void* detour) = 0;
    virtual InstallResult InstallVTable(const TargetInfo& target, void* detour) = 0;
    virtual InstallResult InstallIAT(const TargetInfo& target, void* detour) = 0;

    // Enumerate function return sites for HookPoint::RETURN: offsets relative
    // to the symbol RVA where RET instructions start. The registry installs
    // one breakpoint-trap per site, all dispatching the same chain, so AFTER
    // hooks observe the real return value. Default refuses (clean install error).
    // MUST decode pristine image bytes (read from the image file on disk),
    // never live memory: live code may already carry this process's own
    // patches, and decoding patched bytes desynchronizes the decoder into
    // spurious sites that install cleanly but never fire.
    virtual bool FindReturnOffsets(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
        (void)rva; (void)size; (void)out; return false;
    }

    // Enumerate every instruction start offset of the function at (rva, size),
    // relative to the symbol RVA, for HookPoint::OFFSET boundary validation.
    // Same pristine-bytes requirement as FindReturnOffsets. Default refuses.
    virtual bool FindInstructionBoundaries(uint64_t rva, uint32_t size, std::vector<uint32_t>& out) {
        (void)rva; (void)size; (void)out; return false;
    }

    // Install a breakpoint-trap (int3, 1 byte) at offset_within_symbol for an
    // interior point (HookPoint::OFFSET or one HookPoint::RETURN site
    // enumerated by FindReturnOffsets). A process-wide trap handler
    // (VEH on Windows, SIGTRAP on Linux) dispatches the chain and resumes:
    // OFFSET re-executes the original instruction via a single-step dance,
    // RETURN emulates the ret (see EmulateReturnResume). Unlike SafetyHook
    // mid-hooks this relocates nothing, so it stays correct on functions
    // whose prologue is already patched (ENTRY coexistence) and on code
    // SafetyHook cannot safely steal (e.g. /GS cookie sequences). The 1-byte
    // patch is atomic; installing the same address twice shares the site.
    virtual InstallResult InstallTrap(const TargetInfo& target, uint32_t offset_within_symbol) {
        (void)target; (void)offset_within_symbol;
        return InstallResult{false, "traps not supported", nullptr, nullptr, nullptr};
    }

    virtual bool Remove(HookHandle handle, std::string& error) = 0;

    // Memory primitives, virtualized for testing and fault injection.
    virtual bool ReadMemory(const void* src, void* dst, size_t sz) noexcept = 0;
    virtual bool WriteMemory(void* dst, const void* src, size_t sz) noexcept = 0;
    virtual bool ProtectMemory(void* addr, size_t sz, MemProt new_prot, MemProt* old) noexcept = 0;
    virtual void FlushICache(void* addr, size_t sz) noexcept = 0;

    // Original callable for inline hooks; other points synthesize one with stub ABI.
    using TrampolineFn = void(*)(CpuContext*);

    static std::unique_ptr<HookBackend> CreateForCurrentPlatform();
    static std::unique_ptr<HookBackend> CreateNull(); // no-op for tests / unsupported
};

// Null backend used on unsupported platforms (macOS) or in tests.
class NullHookBackend final : public HookBackend {
public:
    bool IsSupported() const noexcept override { return false; }
    std::string Name() const override { return "null"; }
    void* ImageBase() const noexcept override { return nullptr; }
    BuildId ActiveBuildId() const override { return BuildId{}; }
    InstallResult InstallInline(const TargetInfo&, void*) override { return {false, "backend not supported", nullptr, nullptr, nullptr}; }
    InstallResult InstallVTable(const TargetInfo&, void*) override { return {false, "backend not supported", nullptr, nullptr, nullptr}; }
    InstallResult InstallIAT(const TargetInfo&, void*) override { return {false, "backend not supported", nullptr, nullptr, nullptr}; }
    bool Remove(HookHandle, std::string& e) override { e="no hook"; return false; }
    bool ReadMemory(const void*, void*, size_t) noexcept override { return false; }
    bool WriteMemory(void*, const void*, size_t) noexcept override { return false; }
    bool ProtectMemory(void*, size_t, MemProt, MemProt*) noexcept override { return false; }
    void FlushICache(void*, size_t) noexcept override {}
};

} // namespace Slic3r::Hook
