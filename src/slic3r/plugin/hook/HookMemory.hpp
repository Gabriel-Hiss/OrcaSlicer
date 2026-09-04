#pragma once

#include <cstddef>
#include <cstdint>

namespace Slic3r::Hook {

// Patching and probing primitives over VirtualProtect/mprotect plus icache
// flush. No registry lock is held while callbacks invoke these.

enum class MemProt : uint32_t {
    Read          = 1u << 0,
    Write         = 1u << 1,
    Execute       = 1u << 2,
    ReadWrite     = Read | Write,
    ReadExecute   = Read | Execute,
    ReadWriteExecute = Read | Write | Execute,
};

class HookMemory {
public:
    // Fault-safe copy; false on invalid range. Must not allocate.
    static bool Read(const void* src, void* dst, size_t size) noexcept;
    static bool Write(void* dst, const void* src, size_t size) noexcept;

    // Returns the old protection in out_old when non-null. Never throws.
    static bool Protect(void* addr, size_t size, MemProt new_prot, MemProt* out_old) noexcept;

    // Required after any code patch.
    static void FlushICache(void* addr, size_t size) noexcept;

    // RAII temporary R/W/X around a patch; restores and flushes on exit.
    class ScopedProtect {
    public:
        ScopedProtect(void* addr, size_t size, MemProt new_prot) noexcept
            : addr_(addr), size_(size), ok_(false), old_(MemProt::Read) {
            ok_ = Protect(addr_, size_, new_prot, &old_);
        }
        ~ScopedProtect() noexcept {
            if (ok_) {
                MemProt ignored;
                Protect(addr_, size_, old_, &ignored);
                FlushICache(addr_, size_);
            }
        }
        bool ok() const noexcept { return ok_; }
        ScopedProtect(const ScopedProtect&) = delete;
        ScopedProtect& operator=(const ScopedProtect&) = delete;
    private:
        void* addr_;
        size_t size_;
        bool ok_;
        MemProt old_;
    };

    // OS bit conversion for the host protect_memory bridge.
    static uint32_t ToOsProt(MemProt p) noexcept;
    static MemProt FromOsProt(uint32_t os) noexcept;
};

} // namespace Slic3r::Hook
