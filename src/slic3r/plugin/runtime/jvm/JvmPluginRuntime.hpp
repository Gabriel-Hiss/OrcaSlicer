#pragma once
#include "../../../../../sdk/plugin_v1/abi/orca_hook_api.h"
// One VM per process with an isolated classloader per JAR. Validates Java 25, keeps callbacks alive while hooks exist, and never executes JAR code in OFF builds.
#include <string>
#include <memory>
#include <mutex>
#include <unordered_map>
#include <vector>

#ifdef ORCA_JVM_AVAILABLE
#include <jni.h>
#else
struct JNIEnv_;
struct JavaVM_;
struct _jobject;
using JNIEnv = JNIEnv_;
using JavaVM = JavaVM_;
using jobject = _jobject*;
#endif

namespace Slic3r { namespace Plugin { namespace Jvm {

struct JvmDiscoveryError;
struct LoadedJvmPlugin {
    std::string plugin_id;
    std::string jar_path;
    std::string entry_class;
    jobject classloader_global = nullptr;
    jobject entry_instance_global = nullptr;
    std::vector<jobject> hook_callback_globals; // kept alive while hooks remain
    bool failed = false;
    std::string error;
    std::string stack;
};

class JvmPluginRuntime {
public:
    static JvmPluginRuntime& instance();

    // Idempotent; call once at process start.
    bool ensure_vm(const orca_host_api_v1 *host, std::string &out_error);

    // Load a JAR already inspected (metadata validated, build_id matched) via its isolated URLClassLoader.
    bool load_jar(const std::string &plugin_id, const std::string &jar_path, const std::string &entry_class, std::string &out_error);

    // Unload drains hooks first (caller ensures), then releases JNI refs and closes the classloader.
    bool unload_jar(const std::string &plugin_id, std::string &out_error);

    // Shutdown disarms chains (caller) and releases all loaders.
    void shutdown();

    bool is_available() const { return vm_ != nullptr; }
    bool is_off_build() const;
    JavaVM* vm() const { return vm_; }

    std::shared_ptr<LoadedJvmPlugin> find(const std::string &plugin_id) const;
    std::vector<std::shared_ptr<LoadedJvmPlugin>> all() const;

    void on_hook_throwable(const std::string &plugin_id, const std::string &hook_id, const std::string &message, const std::string &stack);

private:
    JvmPluginRuntime() = default;
    ~JvmPluginRuntime() = default;
    JvmPluginRuntime(const JvmPluginRuntime&) = delete;
    JvmPluginRuntime& operator=(const JvmPluginRuntime&) = delete;

    bool create_vm_from_location(const std::string &jvm_lib, std::string &out_error);
    void* load_jvm_library(const std::string &jvm_lib, std::string &out_error);

    mutable std::mutex mu_;
    JavaVM *vm_ = nullptr;
    void *jvm_handle_ = nullptr;
    std::string jvm_lib_path_;
    const orca_host_api_v1 *host_ = nullptr;
    std::unordered_map<std::string, std::shared_ptr<LoadedJvmPlugin>> plugins_;
    bool off_build_reported_ = false;
};

bool is_jvm_off_build();

}}} // namespace Slic3r::Plugin::Jvm