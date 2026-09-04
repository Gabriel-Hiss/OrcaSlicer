#pragma once
// Deterministic Java 25 discovery: JAVA_HOME, Windows registry, java.exe-relative, Linux PATH symlink.
#include <string>
#include <optional>
#include <vector>

namespace Slic3r { namespace Plugin { namespace Jvm {

struct JvmLocation {
    std::string java_home;   // resolved JAVA_HOME
    std::string jvm_lib;     // absolute path to jvm.dll / libjvm.so
    std::string java_exe;    // java executable if discovered
    int         major = 0;   // validated 25
};

struct DiscoveryError {
    std::string reason;
};

std::optional<JvmLocation> discover_jvm25(DiscoveryError &out_error);

bool validate_java_home_major25(const std::string &java_home, int &out_major, std::string &out_reason);

std::optional<std::string> env_java_home();
std::optional<std::string> read_release_major(const std::string &java_home);
std::string normalize_path(const std::string &p);
bool file_exists(const std::string &p);

#ifdef _WIN32
std::vector<std::string> windows_registry_jdk25_homes();
std::optional<std::string> windows_java_exe_from_path();
std::optional<std::string> windows_jvm_from_java_exe(const std::string &java_exe);
std::optional<std::string> windows_jvm_from_home(const std::string &home);
#else
std::optional<std::string> linux_java_symlink_target();
std::optional<std::string> linux_home_from_java_bin(const std::string &java_bin);
std::optional<std::string> linux_jvm_from_home(const std::string &home);
#endif

}}} // namespace Slic3r::Plugin::Jvm
