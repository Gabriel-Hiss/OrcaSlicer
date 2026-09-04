#include "JvmPluginRuntime.hpp"
#include "JvmDiscovery.hpp"
#include "JvmHostBridge.hpp"

#ifdef ORCA_JVM_AVAILABLE
#include <jni.h>
#endif

#include <algorithm>
#include <cstring>
#include "libslic3r/Utils.hpp"


#include <algorithm>
#include <cstring>

#ifdef _WIN32
#  include <windows.h>
#else
#  include <dlfcn.h>
#endif

namespace Slic3r { namespace Plugin { namespace Jvm {

JvmPluginRuntime& JvmPluginRuntime::instance() {
    static JvmPluginRuntime inst;
    return inst;
}

bool JvmPluginRuntime::is_off_build() const {
#ifdef ORCA_JVM_AVAILABLE
    return false;
#else
    return true;
#endif
}
bool is_jvm_off_build() {
#ifdef ORCA_JVM_AVAILABLE
    return false;
#else
    return true;
#endif
}

#ifdef ORCA_JVM_AVAILABLE

bool JvmPluginRuntime::ensure_vm(const orca_host_api_v1 *host, std::string &out_error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (vm_) { host_ = host; set_host_table(host); return true; }
    host_ = host;
    set_host_table(host);

    DiscoveryError derr;
    auto loc = discover_jvm25(derr);
    if (!loc) {
        out_error = "JVM discovery failed: " + derr.reason + " (requires Java 25)";
        return false;
    }
    if (!create_vm_from_location(loc->jvm_lib, out_error)) return false;

    std::string reg_err;
    if (!register_jni_natives(vm_, host_, [this](const std::string &pid, const std::string &hid, const std::string &msg, const std::string &stack){
        this->on_hook_throwable(pid, hid, msg, stack);
    }, reg_err)) {
        out_error = "JNI bridge registration failed: " + reg_err;
        return false;
    }
    clear_jvm_hook_disables();
    return true;
}

void* JvmPluginRuntime::load_jvm_library(const std::string &jvm_lib, std::string &out_error) {
#ifdef _WIN32
    HMODULE h = LoadLibraryA(jvm_lib.c_str());
    if (!h) {
        DWORD err = GetLastError();
        char buf[512]={0};
        FormatMessageA(FORMAT_MESSAGE_FROM_SYSTEM, nullptr, err, 0, buf, sizeof(buf), nullptr);
        out_error = "LoadLibrary " + jvm_lib + " failed: " + buf;
        return nullptr;
    }
    return (void*)h;
#else
    void *h = dlopen(jvm_lib.c_str(), RTLD_NOW);
    if (!h) {
        out_error = "dlopen " + jvm_lib + " failed: " + (dlerror() ? dlerror() : "unknown");
        return nullptr;
    }
    return h;
#endif
}

bool JvmPluginRuntime::create_vm_from_location(const std::string &jvm_lib, std::string &out_error) {
    // One VM per process; reuse an externally created one.
    jint (*JNI_GetCreatedJavaVMs)(JavaVM**, jsize, jsize*) = nullptr;
    jint (*JNI_CreateJavaVM)(JavaVM**, void**, void*) = nullptr;
#ifdef _WIN32
    void *handle = load_jvm_library(jvm_lib, out_error);
    if (!handle) return false;
    HMODULE h = (HMODULE)handle;
    JNI_GetCreatedJavaVMs = (decltype(JNI_GetCreatedJavaVMs))GetProcAddress(h, "JNI_GetCreatedJavaVMs");
    JNI_CreateJavaVM = (decltype(JNI_CreateJavaVM))GetProcAddress(h, "JNI_CreateJavaVM");
#else
    void *handle = load_jvm_library(jvm_lib, out_error);
    if (!handle) return false;
    JNI_GetCreatedJavaVMs = (decltype(JNI_GetCreatedJavaVMs))dlsym(handle, "JNI_GetCreatedJavaVMs");
    JNI_CreateJavaVM = (decltype(JNI_CreateJavaVM))dlsym(handle, "JNI_CreateJavaVM");
#endif
    if (!JNI_GetCreatedJavaVMs || !JNI_CreateJavaVM) {
        out_error = "jvm lib missing JNI entry points: " + jvm_lib;
        return false;
    }
    JavaVM *existing[1] = {nullptr};
    jsize n = 0;
    if (JNI_GetCreatedJavaVMs(existing, 1, &n) == JNI_OK && n > 0 && existing[0]) {
        vm_ = existing[0];
        jvm_handle_ = handle;
        jvm_lib_path_ = jvm_lib;
        return true;
    }
    JavaVMInitArgs vm_args{};
    JavaVMOption opts[4];
    // Each plugin gets its own URLClassLoader; no custom classpath.
    opts[0].optionString = (char*)"-Djava.awt.headless=true";
    opts[1].optionString = (char*)"-Xcheck:jni";
    // Pass data_dir so plugins can resolve their data directory.
    std::string dataDirOpt = "-Dorca.data_dir=" + Slic3r::data_dir();
    static std::string s_dataDirOptStorage;
    s_dataDirOptStorage = dataDirOpt;
    opts[2].optionString = (char*)s_dataDirOptStorage.c_str();
    vm_args.version = JNI_VERSION_21;
    vm_args.nOptions = 3;
    vm_args.options = opts;
    vm_args.ignoreUnrecognized = JNI_FALSE;
    JNIEnv *env = nullptr;
    jint rc = JNI_CreateJavaVM(&vm_, (void**)&env, &vm_args);
    if (rc != JNI_OK || !vm_) {
        out_error = "JNI_CreateJavaVM failed: " + std::to_string(rc) + " lib=" + jvm_lib;
        return false;
    }
    jvm_handle_ = handle;
    jvm_lib_path_ = jvm_lib;
    return true;
}

bool JvmPluginRuntime::load_jar(const std::string &plugin_id, const std::string &jar_path, const std::string &entry_class, std::string &out_error) {
    std::lock_guard<std::mutex> lk(mu_);
    if (!vm_) { out_error = "JVM not available"; return false; }
    if (plugins_.count(plugin_id)) { out_error = "plugin already loaded: " + plugin_id; return false; }

    JNIEnv *env = attach_current_thread_daemon(vm_);
    if (!env) { out_error = "AttachCurrentThreadAsDaemon failed"; return false; }
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Create an isolated URLClassLoader for this JAR.
    jclass fileCls = env->FindClass("java/io/File");
    jclass urlCls = env->FindClass("java/net/URL");
    jclass urlClCls = env->FindClass("java/net/URLClassLoader");
    jclass classCls = env->FindClass("java/lang/Class");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!fileCls || !urlCls || !urlClCls || !classCls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        out_error = "core classes not found for classloader creation";
        return false;
    }
    jmethodID fileCtor = env->GetMethodID(fileCls, "<init>", "(Ljava/lang/String;)V");
    jmethodID toUri = env->GetMethodID(fileCls, "toURI", "()Ljava/net/URI;");
    jclass uriCls = env->FindClass("java/net/URI");
    jmethodID toUrl = uriCls ? env->GetMethodID(uriCls, "toURL", "()Ljava/net/URL;") : nullptr;
    jmethodID urlClCtor = env->GetMethodID(urlClCls, "<init>", "([Ljava/net/URL;Ljava/lang/ClassLoader;)V");
    jmethodID forName = env->GetStaticMethodID(classCls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!fileCtor || !toUri || !uriCls || !toUrl || !urlClCtor || !forName) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        out_error = "core methods not found for classloader creation";
        return false;
    }

    jstring jpath = env->NewStringUTF(jar_path.c_str());
    if (env->ExceptionCheck() || !jpath) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "NewStringUTF failed for jar_path"; return false; }
    jobject fileObj = env->NewObject(fileCls, fileCtor, jpath);
    if (env->ExceptionCheck() || !fileObj) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "File ctor failed"; env->DeleteLocalRef(jpath); return false; }
    jobject uriObj = env->CallObjectMethod(fileObj, toUri);
    if (env->ExceptionCheck() || !uriObj) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "File.toURI failed"; env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); return false; }
    jobject urlObj = env->CallObjectMethod(uriObj, toUrl);
    if (env->ExceptionCheck() || !urlObj) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "URI.toURL failed"; env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); return false; }
    jclass urlArrCls = env->FindClass("java/net/URL");
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobjectArray urlArr = env->NewObjectArray(1, urlArrCls, urlObj);
    if (env->ExceptionCheck() || !urlArr) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "URL[] creation failed"; env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj); return false; }
    // NativeBridge lives only in the plugin JAR, so fall back to the system loader; each JAR stays isolated.
    jclass bridgeCls = env->FindClass("org/orcaslicer/plugin/v1/NativeBridge");
    jobject parentLoader = nullptr;
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (bridgeCls) {
        jclass clzCls = env->FindClass("java/lang/Class");
        if (clzCls) {
            jmethodID getCl = env->GetMethodID(clzCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
            if (getCl) {
                parentLoader = env->CallObjectMethod(bridgeCls, getCl);
                if (env->ExceptionCheck()) { env->ExceptionClear(); parentLoader = nullptr; }
            }
            env->DeleteLocalRef(clzCls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    } else {
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (!parentLoader) {
        // Fall back to the system classloader for delegation.
        jclass clLoaderCls = env->FindClass("java/lang/ClassLoader");
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (clLoaderCls) {
            jmethodID getSystem = env->GetStaticMethodID(clLoaderCls, "getSystemClassLoader", "()Ljava/lang/ClassLoader;");
            if (getSystem) {
                parentLoader = env->CallStaticObjectMethod(clLoaderCls, getSystem);
                if (env->ExceptionCheck()) { env->ExceptionClear(); parentLoader = nullptr; }
            }
            env->DeleteLocalRef(clLoaderCls);
        }
        if (env->ExceptionCheck()) env->ExceptionClear();
    }
    if (env->ExceptionCheck()) env->ExceptionClear();
    jobject loader = env->NewObject(urlClCls, urlClCtor, urlArr, parentLoader);
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::string stack = throwable_stack_trace(env, thr);
        env->DeleteLocalRef(thr);
        out_error = "URLClassLoader creation failed: " + stack;
        env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj);
        env->DeleteLocalRef(urlArr); if (bridgeCls) env->DeleteLocalRef(bridgeCls);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    jobject loaderGlobal = env->NewGlobalRef(loader);
    if (env->ExceptionCheck()) env->ExceptionClear();

    // Register natives against the plugin's own NativeBridge copy; the system loader does not have it.
    {
        std::string reg_err;
        bool reg_ok = Slic3r::Plugin::Jvm::register_natives_for_loader(env, loader, reg_err);
        std::cout << "[JvmPluginRuntime] register_natives_for_loader for " << plugin_id << " ok=" << reg_ok << " err=" << reg_err << std::endl;
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (!reg_ok) {
            if (host_ && host_->log) host_->log(1, ("register_natives_for_loader failed: " + reg_err).c_str());
            // Continue so orcaRegister reports the failure as a load error.
        }
    }

    jstring jEntry = env->NewStringUTF(entry_class.c_str());
    if (env->ExceptionCheck() || !jEntry) { if (env->ExceptionCheck()) env->ExceptionClear(); out_error = "NewStringUTF failed for entry_class"; if (loaderGlobal) env->DeleteGlobalRef(loaderGlobal); env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj); env->DeleteLocalRef(urlArr); if (bridgeCls) env->DeleteLocalRef(bridgeCls); return false; }
    jclass entryCls = (jclass)env->CallStaticObjectMethod(classCls, forName, jEntry, JNI_TRUE, loader);
    if (env->ExceptionCheck() || !entryCls) {
        jthrowable thr = env->ExceptionOccurred(); if (thr) env->ExceptionClear();
        std::string stack = thr ? throwable_stack_trace(env, thr) : "Class.forName failed";
        if (thr) env->DeleteLocalRef(thr);
        out_error = "entry class not found: " + entry_class + " : " + stack;
        if (loaderGlobal) env->DeleteGlobalRef(loaderGlobal);
        env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj);
        env->DeleteLocalRef(urlArr); if (bridgeCls) env->DeleteLocalRef(bridgeCls);
        env->DeleteLocalRef(jEntry);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }
    // A missing orcaRegister is a hard error; no reflection scan.
    set_loading_plugin_id(plugin_id);
    jmethodID regMid = env->GetStaticMethodID(entryCls, "orcaRegister", "()V");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!regMid) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        if (loaderGlobal) env->DeleteGlobalRef(loaderGlobal);
        env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj);
        env->DeleteLocalRef(urlArr); env->DeleteLocalRef(jEntry); env->DeleteLocalRef(entryCls);
        if (bridgeCls) env->DeleteLocalRef(bridgeCls);
        set_loading_plugin_id("");
        out_error = "missing generated orcaRegister() in entry class " + entry_class + " — build with HookProcessor";
        return false;
    }
    env->CallStaticVoidMethod(entryCls, regMid);
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::string stack = throwable_stack_trace(env, thr);
        std::string msg = throwable_to_string(env, thr);
        env->DeleteLocalRef(thr);
        auto rec = std::make_shared<LoadedJvmPlugin>();
        rec->plugin_id = plugin_id; rec->jar_path = jar_path; rec->entry_class = entry_class;
        rec->classloader_global = loaderGlobal; rec->failed = true; rec->error = msg; rec->stack = stack;
        plugins_[plugin_id] = rec;
        out_error = "orcaRegister threw: " + msg + "\n" + stack;
        env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj);
        env->DeleteLocalRef(urlArr); env->DeleteLocalRef(jEntry); env->DeleteLocalRef(entryCls);
        if (bridgeCls) env->DeleteLocalRef(bridgeCls);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return false;
    }

    set_loading_plugin_id("");
    auto rec = std::make_shared<LoadedJvmPlugin>();
    rec->plugin_id = plugin_id;
    rec->jar_path = jar_path;
    rec->entry_class = entry_class;
    rec->classloader_global = loaderGlobal;
    plugins_[plugin_id] = rec;

    env->DeleteLocalRef(jpath); env->DeleteLocalRef(fileObj); env->DeleteLocalRef(uriObj); env->DeleteLocalRef(urlObj);
    env->DeleteLocalRef(urlArr); env->DeleteLocalRef(jEntry); env->DeleteLocalRef(entryCls);
    if (bridgeCls) env->DeleteLocalRef(bridgeCls);
    env->DeleteLocalRef(fileCls); env->DeleteLocalRef(urlCls); env->DeleteLocalRef(urlClCls); env->DeleteLocalRef(classCls); if (uriCls) env->DeleteLocalRef(uriCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return true;
}


bool JvmPluginRuntime::unload_jar(const std::string &plugin_id, std::string &out_error) {
    std::shared_ptr<LoadedJvmPlugin> rec;
    {
        std::lock_guard<std::mutex> lk(mu_);
        auto it = plugins_.find(plugin_id);
        if (it == plugins_.end()) { out_error = "plugin not loaded: " + plugin_id; return false; }
        rec = it->second;
        plugins_.erase(it);
    }
    remove_all_hooks_for_plugin(plugin_id);
    for (auto cb : rec->hook_callback_globals) safe_delete_global_ref(vm_, cb);
    safe_release_classloader(vm_, rec->classloader_global);
    if (rec->entry_instance_global) safe_delete_global_ref(vm_, rec->entry_instance_global);
    return true;
}

void JvmPluginRuntime::shutdown() {
    std::unordered_map<std::string, std::shared_ptr<LoadedJvmPlugin>> copy;
    {
        std::lock_guard<std::mutex> lk(mu_);
        copy.swap(plugins_);
        clear_jvm_hook_disables();
    }
    for (auto &kv : copy) {
        remove_all_hooks_for_plugin(kv.first);
        for (auto cb : kv.second->hook_callback_globals) safe_delete_global_ref(vm_, cb);
        safe_release_classloader(vm_, kv.second->classloader_global);
        if (kv.second->entry_instance_global) safe_delete_global_ref(vm_, kv.second->entry_instance_global);
    }
    // Never destroy the VM; it lives until process exit.
}

void JvmPluginRuntime::on_hook_throwable(const std::string &plugin_id, const std::string &hook_id, const std::string &message, const std::string &stack) {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(plugin_id);
    if (it != plugins_.end()) { it->second->error = message; it->second->stack = stack; it->second->failed = true; }
    disable_jvm_hook_for_session(plugin_id, hook_id);
    if (host_ && host_->set_error) host_->set_error(message.c_str());
    else if (host_ && host_->log) host_->log(2, ("JVM hook throwable " + plugin_id + "/" + hook_id + ": " + message).c_str());
}

std::shared_ptr<LoadedJvmPlugin> JvmPluginRuntime::find(const std::string &plugin_id) const {
    std::lock_guard<std::mutex> lk(mu_);
    auto it = plugins_.find(plugin_id);
    return it == plugins_.end() ? nullptr : it->second;
}
std::vector<std::shared_ptr<LoadedJvmPlugin>> JvmPluginRuntime::all() const {
    std::lock_guard<std::mutex> lk(mu_);
    std::vector<std::shared_ptr<LoadedJvmPlugin>> out;
    out.reserve(plugins_.size());
    for (auto &kv : plugins_) out.push_back(kv.second);
    return out;
}

#else // OFF build

bool JvmPluginRuntime::ensure_vm(const orca_host_api_v1*, std::string &out_error) {
    out_error = "JVM plugins disabled (ORCA_ENABLE_JVM_PLUGINS=OFF or AUTO without JDK 25)";
    return false;
}
bool JvmPluginRuntime::load_jar(const std::string&, const std::string&, const std::string&, std::string &out_error) {
    out_error = "JVM not compiled (ORCA_ENABLE_JVM_PLUGINS=OFF)";
    return false;
}
bool JvmPluginRuntime::unload_jar(const std::string&, std::string &out_error) { out_error = "JVM not compiled"; return false; }
void JvmPluginRuntime::shutdown() {}
std::shared_ptr<LoadedJvmPlugin> JvmPluginRuntime::find(const std::string&) const { return nullptr; }
std::vector<std::shared_ptr<LoadedJvmPlugin>> JvmPluginRuntime::all() const { return {}; }
void JvmPluginRuntime::on_hook_throwable(const std::string&, const std::string&, const std::string&, const std::string&) {}

#endif

}}} // namespace Slic3r::Plugin::Jvm
