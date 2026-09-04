#pragma once
#include "orca/hook_sdkgen/model.hpp"
#include <string>
namespace orca::hook_sdkgen {
void emit_sdks(const Manifest& manifest, const std::string& out_dir);
void emit_cpp_sdk(const Manifest& manifest, const std::string& sdk_root);
void emit_rust_sdk(const Manifest& manifest, const std::string& sdk_root);
void emit_jvm_sdk(const Manifest& manifest, const std::string& sdk_root);
void emit_zip_and_checksum(const std::string& sdk_root, std::string* out_zip_path);
}
