#pragma once
#include <array>
#include <cstdint>
#include <string>
#include <vector>

namespace orca::patch2plugin {

std::array<uint8_t, 32> sha256(const uint8_t* data, size_t len);
std::array<uint8_t, 32> sha256(const std::vector<uint8_t>& data);
std::array<uint8_t, 32> sha256(const std::string& s);
std::string sha256_hex(const std::array<uint8_t, 32>& h);

} // namespace orca::patch2plugin
