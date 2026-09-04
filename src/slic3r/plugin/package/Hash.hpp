#pragma once

#include <string>
#include <boost/filesystem/path.hpp>

namespace Slic3r::Plugin::Package {

// SHA-256 hex (lowercase, 64 chars) of file contents; empty on error.
std::string sha256_file_hex(const boost::filesystem::path &path, std::string &error);
std::string sha256_bytes_hex(const unsigned char *data, size_t len);
std::string sha256_string_hex(const std::string &s);

bool is_hex64(const std::string &s);

} // namespace Slic3r::Plugin::Package
