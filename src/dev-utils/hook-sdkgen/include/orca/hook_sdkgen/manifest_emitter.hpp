#pragma once
#include "orca/hook_sdkgen/emitter.hpp"

namespace orca::hook_sdkgen {
class ManifestEmitter : public IEmitter {
public:
    void emit(const Manifest& manifest, const std::string& output_path) override;
    std::string name() const override { return "ManifestEmitter"; }
};
} // namespace
