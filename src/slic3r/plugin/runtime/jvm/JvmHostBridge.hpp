#pragma once
#include "../../../../../sdk/plugin_v1/abi/orca_hook_api.h"
#include <string>
#include <functional>

#ifdef ORCA_JVM_AVAILABLE
#include <jni.h>
#else
struct JNIEnv_;
struct JavaVM_;
struct _jobject;
struct _jclass;
struct _jmethodID;
struct _jthrowable;
using JNIEnv = JNIEnv_;
using JavaVM = JavaVM_;
using jobject = _jobject*;
using jclass = _jclass*;
using jmethodID = _jmethodID*;
using jthrowable = _jthrowable*;
#endif

namespace Slic3r { namespace Plugin { namespace Jvm {

using JvmErrorSink = std::function<void(const std::string &plugin_id, const std::string &hook_id, const std::string &message, const std::string &stack)>;

bool register_jni_natives(JavaVM *vm, const orca_host_api_v1 *host, JvmErrorSink sink, std::string &out_error);
bool register_natives_for_loader(JNIEnv *env, jobject loader, std::string &out_error);

JNIEnv* attach_current_thread_daemon(JavaVM *vm);
void detach_current_thread_if_attached(JavaVM *vm);

std::string throwable_to_string(JNIEnv *env, jthrowable thr);
std::string throwable_stack_trace(JNIEnv *env, jthrowable thr);

void safe_delete_global_ref(JavaVM *vm, jobject ref);
void safe_release_classloader(JavaVM *vm, jobject classloader);

const orca_host_api_v1* host_table();
void set_host_table(const orca_host_api_v1 *host);

void disable_jvm_hook_for_session(const std::string &plugin_id, const std::string &hook_id);
bool is_jvm_hook_disabled(const std::string &plugin_id, const std::string &hook_id);
void clear_jvm_hook_disables();
void remove_all_hooks_for_plugin(const std::string &plugin_id);

// Loading scope set by JvmPluginRuntime before calling orcaRegister.
void set_loading_plugin_id(const std::string &pid);
std::string loading_plugin_id();

// Dispatch scope for call_next.
void set_dispatch_next(orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept);
orca_hook_status_t (*dispatch_next())(orca_cpu_context_t*) noexcept;

}}} // namespace Slic3r::Plugin::Jvm
