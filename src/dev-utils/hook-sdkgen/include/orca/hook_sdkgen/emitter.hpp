#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <memory>
#include <string>

namespace orca::hook_sdkgen {

class IEmitter {
public:
    virtual ~IEmitter() = default;
    virtual void emit(const Manifest& manifest, const std::string& output_path) = 0;
    virtual std::string name() const = 0;
};

std::unique_ptr<IEmitter> create_manifest_emitter(); // emits orca-hooks.json
std::unique_ptr<IEmitter> create_binary_emitter();   // emits orca-hooks.bin
std::unique_ptr<IEmitter> create_report_emitter();   // emits report json

} // namespace orca::hook_sdkgen
