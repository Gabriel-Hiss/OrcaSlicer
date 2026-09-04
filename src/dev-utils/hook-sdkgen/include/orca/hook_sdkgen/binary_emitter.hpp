#pragma once
#include "orca/hook_sdkgen/emitter.hpp"

namespace orca::hook_sdkgen {
class BinaryEmitter : public IEmitter {
public:
    void emit(const Manifest& manifest, const std::string& output_path) override;
    std::string name() const override { return "BinaryEmitter"; }
    // Reader helper for runtime validation
    static bool validate_file(const std::string& path, std::string* err);
};
} // namespace
