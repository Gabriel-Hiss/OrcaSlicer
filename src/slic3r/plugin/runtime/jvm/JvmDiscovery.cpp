#include "JvmDiscovery.hpp"

#include <algorithm>
#include <cctype>
#include <cstdio>
#include <cstdlib>
#include <fstream>
#include <sstream>

#ifdef _WIN32
#  include <windows.h>
#  include <fileapi.h>
#else
#  include <unistd.h>
#  include <limits.h>
#  include <sys/stat.h>
#endif

namespace Slic3r { namespace Plugin { namespace Jvm {

std::string normalize_path(const std::string &p) {
    std::string out = p;
    std::replace(out.begin(), out.end(), '\\', '/');
    while (!out.empty() && out.back() == '/') out.pop_back();
    return out;
}

bool file_exists(const std::string &p) {
#ifdef _WIN32
    DWORD attr = GetFileAttributesA(p.c_str());
    return attr != INVALID_FILE_ATTRIBUTES;
#else
    struct stat st{};
    return ::stat(p.c_str(), &st) == 0;
#endif
}

std::optional<std::string> env_java_home() {
    const char *v = std::getenv("JAVA_HOME");
    if (!v || !*v) return std::nullopt;
    std::string s = normalize_path(v);
    if (s.empty()) return std::nullopt;
    return s;
}

std::optional<std::string> read_release_major(const std::string &java_home) {
    std::string release = java_home + "/release";
    std::ifstream f(release);
    if (!f) {
        std::string alt = normalize_path(java_home) + "/release";
        f.open(alt);
        if (!f) return std::nullopt;
    }
    std::string line;
    while (std::getline(f, line)) {
        auto pos = line.find("JAVA_VERSION");
        if (pos == std::string::npos) continue;
        auto eq = line.find('=', pos);
        if (eq == std::string::npos) continue;
        std::string val = line.substr(eq + 1);
        val.erase(std::remove_if(val.begin(), val.end(), [](unsigned char c){ return c=='\"' || c=='\'' || std::isspace(c); }), val.end());
        std::string major;
        for (char c : val) { if (std::isdigit((unsigned char)c)) major.push_back(c); else break; }
        if (!major.empty()) return major;
    }
    return std::nullopt;
}

bool validate_java_home_major25(const std::string &java_home, int &out_major, std::string &out_reason) {
    auto maj_str = read_release_major(java_home);
    if (maj_str) {
        try { out_major = std::stoi(*maj_str); } catch (...) { out_major = 0; }
        if (out_major == 25) return true;
        out_reason = "JAVA_HOME " + java_home + " has major " + *maj_str + " expected 25";
        return false;
    }
    // Fall back to scraping `java -version`.
    std::string java_exe = normalize_path(java_home) + "/bin/java";
#ifdef _WIN32
    java_exe += ".exe";
#endif
    if (!file_exists(java_exe)) {
        out_reason = "no release file and no bin/java at " + java_home;
        return false;
    }
    std::string cmd = "\"" + java_exe + "\" -version 2>&1";
    FILE *pipe = nullptr;
#ifdef _WIN32
    pipe = _popen(cmd.c_str(), "r");
#else
    pipe = popen(cmd.c_str(), "r");
#endif
    if (!pipe) { out_reason = "cannot execute " + java_exe + " -version"; return false; }
    char buf[512] = {0};
    std::string out;
    while (fgets(buf, sizeof(buf), pipe)) out += buf;
#ifdef _WIN32
    _pclose(pipe);
#else
    pclose(pipe);
#endif
    auto q = out.find("\"25");
    if (q != std::string::npos) { out_major = 25; return true; }
    if (out.find(" 25.") != std::string::npos || out.find(" 25-") != std::string::npos || out.find(" 25+") != std::string::npos) {
        out_major = 25; return true;
    }
    out_reason = "java -version did not report major 25: " + out.substr(0, 200);
    return false;
}

#ifdef _WIN32

std::optional<std::string> windows_jvm_from_home(const std::string &home) {
    std::string cand = normalize_path(home) + "/bin/server/jvm.dll";
    std::string cand2 = normalize_path(home) + "/bin/server/jvm.dll";
    if (file_exists(cand)) return cand;
    std::string native = home + "\\bin\\server\\jvm.dll";
    if (file_exists(native)) return native;
    return std::nullopt;
}

std::optional<std::string> windows_jvm_from_java_exe(const std::string &java_exe) {
    std::string exe = normalize_path(java_exe);
    auto slash = exe.rfind('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string bin = exe.substr(0, slash);
    auto slash2 = bin.rfind('/');
    std::string home;
    if (slash2 != std::string::npos) {
        home = exe.substr(0, slash2);
    } else {
        home = bin;
    }
    return windows_jvm_from_home(home);
}

std::optional<std::string> windows_java_exe_from_path() {
    char buf[MAX_PATH * 2] = {0};
    DWORD n = SearchPathA(nullptr, "java.exe", nullptr, (DWORD)sizeof(buf), buf, nullptr);
    if (n == 0 || n >= sizeof(buf)) return std::nullopt;
    return std::string(buf);
}

static std::optional<std::string> reg_read_string(HKEY root, const std::string &sub, const std::string &value) {
    HKEY h = nullptr;
    if (RegOpenKeyExA(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_64KEY, &h) != ERROR_SUCCESS) {
        if (RegOpenKeyExA(root, sub.c_str(), 0, KEY_READ | KEY_WOW64_32KEY, &h) != ERROR_SUCCESS) return std::nullopt;
    }
    char data[1024] = {0};
    DWORD type = 0, len = sizeof(data);
    LONG rc = RegQueryValueExA(h, value.c_str(), nullptr, &type, (LPBYTE)data, &len);
    RegCloseKey(h);
    if (rc != ERROR_SUCCESS || (type != REG_SZ && type != REG_EXPAND_SZ)) return std::nullopt;
    return std::string(data, strnlen(data, len));
}

std::vector<std::string> windows_registry_jdk25_homes() {
    std::vector<std::string> out;
    const std::vector<std::string> bases = {
        "SOFTWARE\\JavaSoft\\JDK\\25",
        "SOFTWARE\\JavaSoft\\JDK\\25.0",
        "SOFTWARE\\Eclipse Adoptium\\JDK\\25",
        "SOFTWARE\\Eclipse Foundation\\JDK\\25",
        "SOFTWARE\\Microsoft\\JDK\\25",
        "SOFTWARE\\BellSoft\\LibericaJDK\\25",
        "SOFTWARE\\Amazon Corretto\\JDK\\25",
    };
    for (auto &b : bases) {
        auto v = reg_read_string(HKEY_LOCAL_MACHINE, b, "JavaHome");
        if (!v) v = reg_read_string(HKEY_LOCAL_MACHINE, b, "InstallationPath");
        if (!v) v = reg_read_string(HKEY_LOCAL_MACHINE, b, "Path");
        if (v && !v->empty()) out.push_back(normalize_path(*v));
        auto v2 = reg_read_string(HKEY_CURRENT_USER, b, "JavaHome");
        if (!v2) v2 = reg_read_string(HKEY_CURRENT_USER, b, "InstallationPath");
        if (v2 && !v2->empty()) out.push_back(normalize_path(*v2));
    }
    HKEY h = nullptr;
    if (RegOpenKeyExA(HKEY_LOCAL_MACHINE, "SOFTWARE\\JavaSoft\\JDK", 0, KEY_READ | KEY_WOW64_64KEY, &h) == ERROR_SUCCESS) {
        char name[256]; DWORD nlen;
        for (DWORD i = 0;; ++i) {
            nlen = sizeof(name);
            if (RegEnumKeyExA(h, i, name, &nlen, nullptr, nullptr, nullptr, nullptr) != ERROR_SUCCESS) break;
            std::string key(name, nlen);
            if (key.rfind("25", 0) == 0) {
                std::string sub = std::string("SOFTWARE\\JavaSoft\\JDK\\") + key;
                auto v = reg_read_string(HKEY_LOCAL_MACHINE, sub, "JavaHome");
                if (v) out.push_back(normalize_path(*v));
            }
        }
        RegCloseKey(h);
    }
    std::vector<std::string> dedup;
    for (auto &p : out) if (std::find(dedup.begin(), dedup.end(), p) == dedup.end()) dedup.push_back(p);
    return dedup;
}

#else // Linux

std::optional<std::string> linux_java_symlink_target() {
    const char *path = std::getenv("PATH");
    if (!path) return std::nullopt;
    std::string spath(path);
    std::istringstream iss(spath);
    std::string dir;
    size_t start = 0;
    while (start <= spath.size()) {
        size_t end = spath.find(':', start);
        if (end == std::string::npos) end = spath.size();
        std::string d = spath.substr(start, end - start);
        if (!d.empty()) {
            std::string cand = d + "/java";
            char resolved[PATH_MAX] = {0};
            ssize_t n = readlink(cand.c_str(), resolved, sizeof(resolved)-1);
            if (n > 0) {
                resolved[n] = '\0';
                std::string target(resolved);
                if (!target.empty() && target[0] != '/') target = d + "/" + target;
                char real[PATH_MAX] = {0};
                if (realpath(target.c_str(), real)) return std::string(real);
                if (realpath(cand.c_str(), real)) return std::string(real);
                return target;
            } else {
                char real[PATH_MAX] = {0};
                struct stat st{};
                if (stat(cand.c_str(), &st) == 0) {
                    if (realpath(cand.c_str(), real)) return std::string(real);
                }
            }
        }
        if (end == spath.size()) break;
        start = end + 1;
    }
    return std::nullopt;
}

std::optional<std::string> linux_home_from_java_bin(const std::string &java_bin) {
    std::string p = normalize_path(java_bin);
    auto slash = p.rfind('/');
    if (slash == std::string::npos) return std::nullopt;
    std::string bin = p.substr(0, slash);
    auto slash2 = bin.rfind('/');
    if (slash2 == std::string::npos) return std::nullopt;
    return p.substr(0, slash2);
}

std::optional<std::string> linux_jvm_from_home(const std::string &home) {
    std::string cand = normalize_path(home) + "/lib/server/libjvm.so";
    if (file_exists(cand)) return cand;
    std::string cand2 = normalize_path(home) + "/lib/amd64/server/libjvm.so";
    if (file_exists(cand2)) return cand2;
    return std::nullopt;
}

#endif

std::optional<JvmLocation> discover_jvm25(DiscoveryError &out_error) {
    if (auto jh = env_java_home()) {
        int maj = 0; std::string reason;
        if (!validate_java_home_major25(*jh, maj, reason)) {
            out_error.reason = reason;
        } else {
#ifdef _WIN32
            if (auto jvm = windows_jvm_from_home(*jh)) {
                return JvmLocation{*jh, *jvm, normalize_path(*jh) + "/bin/java.exe", 25};
            } else {
                out_error.reason = "JAVA_HOME valid 25 but no bin/server/jvm.dll at " + *jh;
            }
#else
            if (auto jvm = linux_jvm_from_home(*jh)) {
                return JvmLocation{*jh, *jvm, normalize_path(*jh) + "/bin/java", 25};
            } else {
                out_error.reason = "JAVA_HOME valid 25 but no lib/server/libjvm.so at " + *jh;
            }
#endif
        }
        // An explicitly set but invalid JAVA_HOME fails discovery without probing further.
        if (jh) {
            if (out_error.reason.empty()) out_error.reason = "JAVA_HOME invalid";
            return std::nullopt;
        }
    }

#ifdef _WIN32
    for (auto &home : windows_registry_jdk25_homes()) {
        int maj = 0; std::string reason;
        if (!validate_java_home_major25(home, maj, reason)) continue;
        if (auto jvm = windows_jvm_from_home(home)) {
            return JvmLocation{home, *jvm, normalize_path(home) + "/bin/java.exe", 25};
        }
    }
    if (auto exe = windows_java_exe_from_path()) {
        if (auto jvm = windows_jvm_from_java_exe(*exe)) {
            std::string exe_n = normalize_path(*exe);
            auto slash = exe_n.rfind('/');
            std::string bin = (slash == std::string::npos) ? "" : exe_n.substr(0, slash);
            auto slash2 = bin.rfind('/');
            std::string home = (slash2 == std::string::npos) ? bin : exe_n.substr(0, slash2);
            int maj = 0; std::string reason;
            if (validate_java_home_major25(home, maj, reason)) {
                return JvmLocation{home, *jvm, *exe, 25};
            } else {
                out_error.reason = reason;
                return std::nullopt;
            }
        }
    }
    out_error.reason = out_error.reason.empty() ? "no Java 25 found via JAVA_HOME, registry or PATH" : out_error.reason;
    return std::nullopt;
#else
    if (auto java_bin = linux_java_symlink_target()) {
        if (auto home = linux_home_from_java_bin(*java_bin)) {
            int maj = 0; std::string reason;
            if (!validate_java_home_major25(*home, maj, reason)) {
                out_error.reason = reason;
                return std::nullopt;
            }
            if (auto jvm = linux_jvm_from_home(*home)) {
                return JvmLocation{*home, *jvm, *java_bin, 25};
            } else {
                out_error.reason = "java home " + *home + " has no lib/server/libjvm.so";
                return std::nullopt;
            }
        }
    }
    out_error.reason = out_error.reason.empty() ? "no Java 25 found via JAVA_HOME or PATH" : out_error.reason;
    return std::nullopt;
#endif
}

}}} // namespace Slic3r::Plugin::Jvm
