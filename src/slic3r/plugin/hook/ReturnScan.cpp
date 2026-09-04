#include "HookBackend.hpp"
#include <algorithm>

#if __has_include(<Zydis.h>)
#include <Zydis.h>
#define HOOK_HAS_ZYDIS 1
#else
#define HOOK_HAS_ZYDIS 0
#endif

namespace Slic3r::Hook {

bool ScanReturnOffsets(const uint8_t* bytes, size_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (bytes == nullptr || size == 0)
        return false;
#if HOOK_HAS_ZYDIS
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return false;
    // Sanity cap: a single function instrumented with one mid-hook per RET.
    // More sites than this almost certainly means mis-decoded data, and the
    // mid-hook slot table (128 per process) could not hold them anyway.
    constexpr size_t kMaxRetSites = 64;
    size_t offset = 0;
    while (offset < size) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes + offset, size - offset, &insn, operands)))
            return false;
        if (insn.length == 0)
            return false;
        if (insn.mnemonic == ZYDIS_MNEMONIC_RET) {
            if (out.size() >= kMaxRetSites)
                return false;
            out.push_back(static_cast<uint32_t>(offset));
        }
        offset += insn.length;
    }
    return !out.empty();
#else
    return false;
#endif
}

bool ScanInstructionBoundaries(const uint8_t* bytes, size_t size, std::vector<uint32_t>& out) {
    out.clear();
    if (bytes == nullptr || size == 0)
        return false;
#if HOOK_HAS_ZYDIS
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return false;
    size_t offset = 0;
    while (offset < size) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, bytes + offset, size - offset, &insn, operands)))
            return false;
        if (insn.length == 0)
            return false;
        out.push_back(static_cast<uint32_t>(offset));
        offset += insn.length;
    }
    return !out.empty();
#else
    return false;
#endif
}

uint32_t FindNearestInstructionBoundary(const std::vector<uint32_t>& bounds, uint32_t want) {
    if (bounds.empty())
        return want;
    auto it = std::lower_bound(bounds.begin(), bounds.end(), want);
    if (it == bounds.end())
        return bounds.back();
    if (*it == want)
        return want;
    uint32_t hi = *it;
    if (it == bounds.begin())
        return hi;
    uint32_t lo = *(it - 1);
    return (want - lo <= hi - want) ? lo : hi;
}

bool EmulateReturnResume(const uint8_t* code, size_t len, uint64_t stacked_retaddr,
                         uint64_t rsp, uint64_t& out_rip, uint64_t& out_rsp) {
    if (code == nullptr || len == 0)
        return false;
#if HOOK_HAS_ZYDIS
    ZydisDecoder decoder;
    if (!ZYAN_SUCCESS(ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64)))
        return false;
    ZydisDecodedInstruction insn;
    ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
    if (!ZYAN_SUCCESS(ZydisDecoderDecodeFull(&decoder, code, len, &insn, operands)))
        return false;
    if (insn.mnemonic != ZYDIS_MNEMONIC_RET)
        return false;
    // Near/far plain ret pops the return address (8 bytes in 64-bit).
    // The imm16 forms (C2/CA) additionally release imm bytes of arguments.
    uint64_t pop = 8;
    if (insn.length == 3 && len >= 3 && (code[0] == 0xC2 || code[0] == 0xCA))
        pop += static_cast<uint64_t>(code[1]) | (static_cast<uint64_t>(code[2]) << 8);
    out_rip = stacked_retaddr;
    out_rsp = rsp + pop;
    return true;
#else
    (void)stacked_retaddr; (void)rsp; (void)out_rip; (void)out_rsp;
    return false;
#endif
}

} // namespace Slic3r::Hook
