#pragma once
#include <string>
namespace Slic3r { namespace Plugin { namespace Jvm {
inline bool is_jvm_available() {
#ifdef ORCA_JVM_AVAILABLE
    return true;
#else
    return false;
#endif
}
inline std::string jvm_unavailable_reason() {
    return "JVM plugins disabled (ORCA_ENABLE_JVM_PLUGINS=OFF or AUTO without JDK 25)";
}
bool inspect_jar_metadata(const std::string &jar_path, std::string &out_id, std::string &out_entry_class, std::string &out_error);
}}} // namespace Slic3r::Plugin::Jvm
