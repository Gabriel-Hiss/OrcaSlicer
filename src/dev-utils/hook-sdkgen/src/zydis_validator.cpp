#include "orca/hook_sdkgen/zydis_validator.hpp"

#ifndef HAVE_ZYDIS
#if __has_include(<Zydis.h>)
#define HAVE_ZYDIS 1
#include <Zydis.h>
#elif __has_include(<Zydis/Zydis.h>)
#define HAVE_ZYDIS 1
#include <Zydis/Zydis.h>
#else
#define HAVE_ZYDIS 0
#endif
#else
// HAVE_ZYDIS already defined by CMake
#if HAVE_ZYDIS
#if __has_include(<Zydis.h>)
#include <Zydis.h>
#elif __has_include(<Zydis/Zydis.h>)
#include <Zydis/Zydis.h>
#endif
#endif
#endif

#include <string>

namespace orca::hook_sdkgen {

ZydisStatus zydis_status() {
#if HAVE_ZYDIS
    // Zydis exposes the version as a packed ZyanU64 plus field-extraction macros.
    const ZyanU64 packed = ZydisGetVersion();
    std::string ver = std::to_string(static_cast<unsigned>(ZYDIS_VERSION_MAJOR(packed))) + "." +
                      std::to_string(static_cast<unsigned>(ZYDIS_VERSION_MINOR(packed))) + "." +
                      std::to_string(static_cast<unsigned>(ZYDIS_VERSION_PATCH(packed)));
    return {true, ver, ""};
#else
    return {false, "", "Zydis headers not found; instruction validation disabled"};
#endif
}

std::vector<Instruction> validate_instructions(const uint8_t* bytes, size_t len, uint32_t /*base_rva*/, std::string* error) {
    std::vector<Instruction> out;
#if HAVE_ZYDIS
    ZydisDecoder decoder;
    ZydisDecoderInit(&decoder, ZYDIS_MACHINE_MODE_LONG_64, ZYDIS_STACK_WIDTH_64);
    ZydisFormatter formatter;
    ZydisFormatterInit(&formatter, ZYDIS_FORMATTER_STYLE_INTEL);
    size_t offset = 0;
    while (offset < len) {
        ZydisDecodedInstruction insn;
        ZydisDecodedOperand operands[ZYDIS_MAX_OPERAND_COUNT];
        ZyanStatus status = ZydisDecoderDecodeFull(&decoder, bytes + offset, len - offset, &insn, operands);
        if (!ZYAN_SUCCESS(status)) {
            if (error) *error = "Zydis decode failed at offset " + std::to_string(offset);
            break;
        }
        Instruction ii;
        ii.offset = (uint32_t)offset;
        ii.size = insn.length;
        if (insn.mnemonic == ZYDIS_MNEMONIC_CALL) ii.is_call = true;
        if (insn.mnemonic == ZYDIS_MNEMONIC_RET) ii.is_ret = true;
        out.push_back(ii);
        offset += insn.length;
        if (insn.length == 0) { if (error) *error = "zero-length instruction"; break; }
    }
#else
    (void)bytes; (void)len;
    if (error) *error = "Zydis not available";
#endif
    return out;
}

bool is_instruction_boundary(const std::vector<Instruction>& instrs, uint32_t offset) {
    for (auto& i : instrs) if (i.offset == offset) return true;
    return false;
}

} // namespace
