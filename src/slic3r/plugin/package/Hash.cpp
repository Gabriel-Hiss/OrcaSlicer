#include "Hash.hpp"

#include <cstdio>
#include <fstream>
#include <iomanip>
#include <sstream>

#include <openssl/sha.h>
#include <boost/nowide/fstream.hpp>

namespace Slic3r::Plugin::Package {

std::string sha256_bytes_hex(const unsigned char *data, size_t len)
{
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256(data, len, out);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) oss << std::setw(2) << (int)out[i];
    return oss.str();
}

std::string sha256_string_hex(const std::string &s)
{
    return sha256_bytes_hex(reinterpret_cast<const unsigned char*>(s.data()), s.size());
}

bool is_hex64(const std::string &s)
{
    if (s.size() != 64) return false;
    for (char c : s) {
        if (!((c >= '0' && c <= '9') || (c >= 'a' && c <= 'f') || (c >= 'A' && c <= 'F'))) return false;
    }
    return true;
}

std::string sha256_file_hex(const boost::filesystem::path &path, std::string &error)
{
    boost::nowide::ifstream f(path.string(), std::ios::binary);
    if (!f) {
        error = "cannot open file for hashing: " + path.string();
        return {};
    }
    SHA256_CTX ctx;
    SHA256_Init(&ctx);
    char buf[8192];
    while (f.good()) {
        f.read(buf, sizeof(buf));
        std::streamsize n = f.gcount();
        if (n > 0) SHA256_Update(&ctx, buf, (size_t)n);
        if (f.bad()) {
            error = "read error while hashing: " + path.string();
            return {};
        }
    }
    unsigned char out[SHA256_DIGEST_LENGTH];
    SHA256_Final(out, &ctx);
    std::ostringstream oss;
    oss << std::hex << std::setfill('0');
    for (int i = 0; i < SHA256_DIGEST_LENGTH; ++i) oss << std::setw(2) << (int)out[i];
    return oss.str();
}

} // namespace Slic3r::Plugin::Package
