#include "JvmHostBridge.hpp"

#ifdef ORCA_JVM_AVAILABLE
#include <jni.h>
#include <mutex>
#include <unordered_map>
#include <string>
#include <cstring>
#include <iostream>
#include "libslic3r/Utils.hpp"

namespace Slic3r { namespace Plugin { namespace Jvm {

static std::string loader_to_string_diag(JNIEnv *env, jobject loader) {
    if (!loader) return "null";
    jclass loCls = env->GetObjectClass(loader);
    if (!loCls) { if (env->ExceptionCheck()) env->ExceptionClear(); return "loader-class-not-found"; }
    jmethodID toStr = env->GetMethodID(loCls, "toString", "()Ljava/lang/String;");
    if (!toStr) { env->DeleteLocalRef(loCls); if (env->ExceptionCheck()) env->ExceptionClear(); return "no-toString"; }
    jstring js = (jstring)env->CallObjectMethod(loader, toStr);
    std::string out;
    if (env->ExceptionCheck()) { env->ExceptionClear(); out = "exception-in-toString"; }
    else if (js) {
        const char* c = env->GetStringUTFChars(js, nullptr);
        if (c) out = c;
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    } else out = "null-toString";
    env->DeleteLocalRef(loCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return out;
}
static std::string class_loader_string_diag(JNIEnv *env, jclass cls) {
    if (!cls) return "null-class";
    jclass clCls = env->FindClass("java/lang/Class");
    if (!clCls) { if (env->ExceptionCheck()) env->ExceptionClear(); return "Class-not-found"; }
    jmethodID getCl = env->GetMethodID(clCls, "getClassLoader", "()Ljava/lang/ClassLoader;");
    if (!getCl) { env->DeleteLocalRef(clCls); if (env->ExceptionCheck()) env->ExceptionClear(); return "no-getClassLoader"; }
    jobject cl = env->CallObjectMethod(cls, getCl);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(clCls); return "exception-getClassLoader"; }
    std::string s = loader_to_string_diag(env, cl);
    if (cl) env->DeleteLocalRef(cl);
    env->DeleteLocalRef(clCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return s;
}

static const orca_host_api_v1 *g_host = nullptr;
static JvmErrorSink g_sink;
static std::mutex g_mu;
static std::unordered_set<std::string> g_disabled;

struct HookRecord {
    orca_hook_handle_t handle = nullptr;
    JavaVM *vm = nullptr;
    jobject globalCallback = nullptr;
    jmethodID mid = nullptr;
    std::string plugin_id;
    std::string hook_id;
};

static std::mutex g_hookMu;
static std::unordered_map<std::string, HookRecord> g_hooks; // key plugin\0hook

static thread_local std::string t_loadingPlugin;
static thread_local orca_hook_status_t (*t_next)(orca_cpu_context_t*) noexcept = nullptr;

void set_loading_plugin_id(const std::string &pid) { t_loadingPlugin = pid; }
std::string loading_plugin_id() { return t_loadingPlugin; }
void set_dispatch_next(orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept) { t_next = next; }
orca_hook_status_t (*dispatch_next())(orca_cpu_context_t*) noexcept { return t_next; }

const orca_host_api_v1* host_table() { return g_host; }
void set_host_table(const orca_host_api_v1 *h) { g_host = h; }

void disable_jvm_hook_for_session(const std::string &pid, const std::string &hid) {
    std::lock_guard<std::mutex> lk(g_mu);
    g_disabled.insert(pid + '\0' + hid);
}
bool is_jvm_hook_disabled(const std::string &pid, const std::string &hid) {
    std::lock_guard<std::mutex> lk(g_mu);
    return g_disabled.find(pid + '\0' + hid) != g_disabled.end();
}
void clear_jvm_hook_disables() {
    std::lock_guard<std::mutex> lk(g_mu);
    g_disabled.clear();
}

JNIEnv* attach_current_thread_daemon(JavaVM *vm) {
    JNIEnv *env = nullptr;
    jint rc = vm->GetEnv((void**)&env, JNI_VERSION_21);
    if (rc == JNI_EDETACHED) {
        JavaVMAttachArgs args{};
        args.version = JNI_VERSION_21;
        if (vm->AttachCurrentThreadAsDaemon((void**)&env, &args) != JNI_OK) return nullptr;
        return env;
    }
    return env;
}
void detach_current_thread_if_attached(JavaVM *) {}

std::string throwable_to_string(JNIEnv *env, jthrowable thr) {
    if (!thr) return {};
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass thrCls = env->GetObjectClass(thr);
    if (!thrCls || env->ExceptionCheck()) { if (env->ExceptionCheck()) env->ExceptionClear(); return "Throwable"; }
    jmethodID toStr = env->GetMethodID(thrCls, "toString", "()Ljava/lang/String;");
    if (!toStr || env->ExceptionCheck()) { if (env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(thrCls); return "Throwable"; }
    jstring js = (jstring)env->CallObjectMethod(thr, toStr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(thrCls); return "Throwable.toString failed"; }
    if (!js) { env->DeleteLocalRef(thrCls); return "Throwable"; }
    const char *c = env->GetStringUTFChars(js, nullptr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(js); env->DeleteLocalRef(thrCls); return "Throwable.toString failed"; }
    std::string s = c ? c : "Throwable";
    if (c) env->ReleaseStringUTFChars(js, c);
    env->DeleteLocalRef(js);
    env->DeleteLocalRef(thrCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    return s;
}
std::string throwable_stack_trace(JNIEnv *env, jthrowable thr) {
    if (!thr) return {};
    if (env->ExceptionCheck()) env->ExceptionClear();
    jclass swCls = env->FindClass("java/io/StringWriter");
    jclass pwCls = env->FindClass("java/io/PrintWriter");
    jclass thCls = env->FindClass("java/lang/Throwable");
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (!swCls || !pwCls || !thCls) {
        if (swCls) env->DeleteLocalRef(swCls);
        if (pwCls) env->DeleteLocalRef(pwCls);
        if (thCls) env->DeleteLocalRef(thCls);
        if (env->ExceptionCheck()) env->ExceptionClear();
        return throwable_to_string(env, thr);
    }
    jmethodID swCtor = env->GetMethodID(swCls, "<init>", "()V");
    jmethodID pwCtor = env->GetMethodID(pwCls, "<init>", "(Ljava/io/Writer;)V");
    jmethodID printStack = env->GetMethodID(thCls, "printStackTrace", "(Ljava/io/PrintWriter;)V");
    jmethodID swToStr = env->GetMethodID(swCls, "toString", "()Ljava/lang/String;");
    if (env->ExceptionCheck() || !swCtor || !pwCtor || !printStack || !swToStr) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(swCls); env->DeleteLocalRef(pwCls); env->DeleteLocalRef(thCls);
        return throwable_to_string(env, thr);
    }
    jobject sw = env->NewObject(swCls, swCtor);
    if (env->ExceptionCheck() || !sw) { if (env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(swCls); env->DeleteLocalRef(pwCls); env->DeleteLocalRef(thCls); return throwable_to_string(env, thr); }
    jobject pw = env->NewObject(pwCls, pwCtor, sw);
    if (env->ExceptionCheck() || !pw) { if (env->ExceptionCheck()) env->ExceptionClear(); env->DeleteLocalRef(sw); env->DeleteLocalRef(swCls); env->DeleteLocalRef(pwCls); env->DeleteLocalRef(thCls); return throwable_to_string(env, thr); }
    env->CallVoidMethod(thr, printStack, pw);
    if (env->ExceptionCheck()) env->ExceptionClear();
    jstring js = (jstring)env->CallObjectMethod(sw, swToStr);
    if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(pw); env->DeleteLocalRef(sw); env->DeleteLocalRef(swCls); env->DeleteLocalRef(pwCls); env->DeleteLocalRef(thCls); return throwable_to_string(env, thr); }
    std::string out;
    if (js) {
        const char *c = env->GetStringUTFChars(js, nullptr);
        if (env->ExceptionCheck()) { env->ExceptionClear(); env->DeleteLocalRef(js); env->DeleteLocalRef(pw); env->DeleteLocalRef(sw); env->DeleteLocalRef(swCls); env->DeleteLocalRef(pwCls); env->DeleteLocalRef(thCls); return throwable_to_string(env, thr); }
        if (c) out = c;
        if (c) env->ReleaseStringUTFChars(js, c);
        env->DeleteLocalRef(js);
    }
    env->DeleteLocalRef(pw);
    env->DeleteLocalRef(sw);
    env->DeleteLocalRef(swCls);
    env->DeleteLocalRef(pwCls);
    env->DeleteLocalRef(thCls);
    if (env->ExceptionCheck()) env->ExceptionClear();
    if (out.empty()) out = throwable_to_string(env, thr);
    return out;
}


void safe_delete_global_ref(JavaVM *vm, jobject ref) {
    if (!ref || !vm) return;
    JNIEnv *env = nullptr;
    if (vm->GetEnv((void**)&env, JNI_VERSION_21) == JNI_EDETACHED) env = attach_current_thread_daemon(vm);
    if (!env) return;
    env->DeleteGlobalRef(ref);
    if (env->ExceptionCheck()) env->ExceptionClear();
}
void safe_release_classloader(JavaVM *vm, jobject loader) {
    if (!loader || !vm) return;
    JNIEnv *env = attach_current_thread_daemon(vm);
    if (!env) { safe_delete_global_ref(vm, loader); return; }
    jclass clCls = env->GetObjectClass(loader);
    if (clCls) {
        jmethodID closeMid = env->GetMethodID(clCls, "close", "()V");
        if (closeMid) { env->CallVoidMethod(loader, closeMid); if (env->ExceptionCheck()) env->ExceptionClear(); }
        env->DeleteLocalRef(clCls);
    }
    env->DeleteGlobalRef(loader);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static orca_hook_status_t JNICALL jvm_hook_trampoline(orca_cpu_context_t *ctx, orca_hook_result_t *out_result, void *user_data) noexcept {
    HookRecord *rec = static_cast<HookRecord*>(user_data);
    if (!rec || !rec->globalCallback || !rec->vm) return ORCA_HOOK_ERR_INTERNAL;
    if (is_jvm_hook_disabled(rec->plugin_id, rec->hook_id)) return ORCA_HOOK_OK;
    JNIEnv *env = attach_current_thread_daemon(rec->vm);
    if (!env) return ORCA_HOOK_ERR_JVM_UNAVAILABLE;
    if (!rec->mid) {
        jclass cbCls = env->GetObjectClass(rec->globalCallback);
        rec->mid = env->GetMethodID(cbCls, "handle", "(JJ)V");
        if (!rec->mid) rec->mid = env->GetMethodID(cbCls, "invoke", "(JJ)V");
        if (!rec->mid) rec->mid = env->GetMethodID(cbCls, "onHook", "(JJ)V");
        env->DeleteLocalRef(cbCls);
        if (!rec->mid) {
            if (env->ExceptionCheck()) env->ExceptionClear();
            return ORCA_HOOK_ERR_INTERNAL;
        }
    }
    if (out_result) { out_result->size = sizeof(orca_hook_result_t); out_result->version = ORCA_HOOK_ABI_VERSION; out_result->action = ORCA_HOOK_ACTION_CONTINUE; }
    t_next = nullptr;
    env->CallVoidMethod(rec->globalCallback, rec->mid, (jlong)(intptr_t)ctx, (jlong)(intptr_t)out_result);
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::string msg = throwable_to_string(env, thr);
        std::string stack = throwable_stack_trace(env, thr);
        env->DeleteLocalRef(thr);
        disable_jvm_hook_for_session(rec->plugin_id, rec->hook_id);
        if (g_sink) g_sink(rec->plugin_id, rec->hook_id, msg, stack);
        if (g_host && g_host->set_error) g_host->set_error(msg.c_str());
        if (g_host && g_host->log) g_host->log(2, msg.c_str());
        return ORCA_HOOK_OK;
    }
    return ORCA_HOOK_OK;
}

static orca_hook_status_t JNICALL jvm_replace_trampoline(orca_cpu_context_t *ctx, orca_hook_result_t *out_result, orca_hook_status_t (*next)(orca_cpu_context_t*) noexcept, void *user_data) noexcept {
    HookRecord *rec = static_cast<HookRecord*>(user_data);
    if (!rec || !rec->globalCallback || !rec->vm) return ORCA_HOOK_ERR_INTERNAL;
    if (is_jvm_hook_disabled(rec->plugin_id, rec->hook_id)) {
        if (next) return next(ctx);
        return ORCA_HOOK_OK;
    }
    JNIEnv *env = attach_current_thread_daemon(rec->vm);
    if (!env) return ORCA_HOOK_ERR_JVM_UNAVAILABLE;
    if (!rec->mid) {
        jclass cbCls = env->GetObjectClass(rec->globalCallback);
        rec->mid = env->GetMethodID(cbCls, "handle", "(JJ)V");
        if (!rec->mid) rec->mid = env->GetMethodID(cbCls, "invoke", "(JJ)V");
        if (!rec->mid) rec->mid = env->GetMethodID(cbCls, "onHook", "(JJ)V");
        env->DeleteLocalRef(cbCls);
        if (!rec->mid) { if (env->ExceptionCheck()) env->ExceptionClear(); return ORCA_HOOK_ERR_INTERNAL; }
    }
    if (out_result) { out_result->size = sizeof(orca_hook_result_t); out_result->version = ORCA_HOOK_ABI_VERSION; out_result->action = ORCA_HOOK_ACTION_CONTINUE; }
    t_next = next;
    env->CallVoidMethod(rec->globalCallback, rec->mid, (jlong)(intptr_t)ctx, (jlong)(intptr_t)out_result);
    t_next = nullptr;
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::string msg = throwable_to_string(env, thr);
        std::string stack = throwable_stack_trace(env, thr);
        env->DeleteLocalRef(thr);
        disable_jvm_hook_for_session(rec->plugin_id, rec->hook_id);
        if (g_sink) g_sink(rec->plugin_id, rec->hook_id, msg, stack);
        if (g_host && g_host->set_error) g_host->set_error(msg.c_str());
        if (g_host && g_host->log) g_host->log(2, msg.c_str());
        return ORCA_HOOK_OK;
    }
    return ORCA_HOOK_OK;
}

static jint JNICALL native_installHook(JNIEnv *env, jclass, jstring j_hookId, jlong targetIdOrRva, jint point, jint kind, jint priority, jobject callback) {
    if (!g_host || !g_host->install_hook) return ORCA_HOOK_ERR_INTERNAL;
    if (!j_hookId || !callback) return ORCA_HOOK_ERR_INVALID_ARG;
    const char *cHook = env->GetStringUTFChars(j_hookId, nullptr);
    std::string hookIdFull = cHook ? cHook : "";
    if (cHook) env->ReleaseStringUTFChars(j_hookId, cHook);
    if (hookIdFull.empty()) return ORCA_HOOK_ERR_INVALID_ARG;

    std::string hookId;
    std::string targetId;
    size_t sep = hookIdFull.find('|');
    if (sep != std::string::npos) {
        hookId = hookIdFull.substr(0, sep);
        targetId = hookIdFull.substr(sep + 1);
        if (hookId.empty() || targetId.empty()) return ORCA_HOOK_ERR_INVALID_ARG;
    } else {
        hookId = hookIdFull;
        targetId = hookIdFull; // legacy: hookId == target
    }
    std::string pluginId = t_loadingPlugin;
    if (pluginId.empty()) {
        return ORCA_HOOK_ERR_INVALID_ARG;
    }
    std::string key = pluginId + '\0' + hookId;

    jobject globalCb = env->NewGlobalRef(callback);
    if (!globalCb) return ORCA_HOOK_ERR_INTERNAL;
    if (env->ExceptionCheck()) env->ExceptionClear();

    HookRecord rec;
    rec.vm = nullptr;
    env->GetJavaVM(&rec.vm);
    rec.globalCallback = globalCb;
    rec.plugin_id = pluginId;
    rec.hook_id = hookId;

    orca_hook_request_t req = orca_hook::make_hook_request();
    req.hook_id = nullptr;
    char *hookIdCopy = new char[hookId.size()+1]; std::memcpy(hookIdCopy, hookId.c_str(), hookId.size()+1);
    req.hook_id = hookIdCopy;
    // For OFFSET points the target id is a symbol and targetIdOrRva is its absolute offset.
    if ((orca_hook_point_t)point == ORCA_HOOK_POINT_OFFSET) {
        if (targetId.empty() || targetIdOrRva == 0) return ORCA_HOOK_ERR_INVALID_ARG;
        char *tcopy = new char[targetId.size()+1]; std::memcpy(tcopy, targetId.c_str(), targetId.size()+1);
        req.target_symbol_id = tcopy;
        req.target_rva = 0;
        req.u.offset.rva = (uint64_t)targetIdOrRva;
    } else if (targetIdOrRva == 0) {
        char *tcopy = new char[targetId.size()+1]; std::memcpy(tcopy, targetId.c_str(), targetId.size()+1);
        req.target_symbol_id = tcopy;
        req.target_rva = 0;
    } else {
        if (!targetId.empty() && targetId != hookId) {
            char *tcopy = new char[targetId.size()+1]; std::memcpy(tcopy, targetId.c_str(), targetId.size()+1);
            req.target_symbol_id = tcopy;
            req.target_rva = (uint64_t)targetIdOrRva;
        } else {
            req.target_symbol_id = nullptr;
            req.target_rva = (uint64_t)targetIdOrRva;
        }
    }
    req.point = (orca_hook_point_t)point;
    req.kind = (orca_hook_kind_t)kind;
    req.priority = (uint32_t)priority;
    {
        std::lock_guard<std::mutex> lk(g_hookMu);
        auto &slot = g_hooks[key];
        slot = rec;
        HookRecord *ud = &g_hooks[key];
        if (req.kind == ORCA_HOOK_KIND_REPLACE) {
            req.callback = (void*)jvm_replace_trampoline;
        } else {
            req.callback = (void*)jvm_hook_trampoline;
        }
        req.user_data = (void*)ud;
        orca_hook_handle_t h = nullptr;
        orca_hook_status_t st = g_host->install_hook(&req, &h);
        if (st != ORCA_HOOK_OK) {
            env->DeleteGlobalRef(globalCb);
            delete[] hookIdCopy;
            if (req.target_symbol_id) delete[] (char*)req.target_symbol_id;
            g_hooks.erase(key);
            return (jint)st;
        }
        ud->handle = h;
    }
    return ORCA_HOOK_OK;
}

static void JNICALL native_log(JNIEnv *env, jclass, jstring j_msg) {
    if (!g_host || !g_host->log) return;
    if (!j_msg) return;
    const char *c = env->GetStringUTFChars(j_msg, nullptr);
    std::string s = c ? c : "";
    if (c) env->ReleaseStringUTFChars(j_msg, c);
    g_host->log(0, s.c_str());
}
static jboolean JNICALL native_callNext(JNIEnv *, jclass, jlong ctxPtr) {
    auto next = t_next;
    if (!next) return JNI_FALSE;
    orca_cpu_context_t *ctx = (orca_cpu_context_t*)(intptr_t)ctxPtr;
    if (!ctx) return JNI_FALSE;
    orca_hook_status_t st = next(ctx);
    return st == ORCA_HOOK_OK ? JNI_TRUE : JNI_FALSE;
}
static jstring JNICALL native_getDataDir(JNIEnv *env, jclass) {
    try {
        std::string d = Slic3r::data_dir();
        return env->NewStringUTF(d.c_str());
    } catch (...) {
        return nullptr;
    }
}
static jbyte JNICALL native_readU8(JNIEnv *, jclass, jlong addr) {
    if (!addr) return 0;
    return *(jbyte*)(intptr_t)addr;
}
static jint JNICALL native_readU32(JNIEnv *, jclass, jlong addr) {
    if (!addr) return 0;
    return *(jint*)(intptr_t)addr;
}
static jlong JNICALL native_readU64(JNIEnv *, jclass, jlong addr) {
    if (!addr) return 0;
    return *(jlong*)(intptr_t)addr;
}
static void JNICALL native_writeU64(JNIEnv *, jclass, jlong addr, jlong value) {
    if (!addr) return;
    *(jlong*)(intptr_t)addr = value;
}
static void JNICALL native_writeBytes(JNIEnv *env, jclass, jlong addr, jbyteArray data) {
    if (!addr || !data) return;
    jsize len = env->GetArrayLength(data);
    if (len <= 0) return;
    jbyte* elems = env->GetByteArrayElements(data, nullptr);
    if (!elems) return;
    std::memcpy((void*)(intptr_t)addr, elems, (size_t)len);
    env->ReleaseByteArrayElements(data, elems, JNI_ABORT);
    if (env->ExceptionCheck()) env->ExceptionClear();
}

static JNINativeMethod g_methods[] = {
    {"nativeInstallHook", "(Ljava/lang/String;JIIILjava/lang/Object;)I", (void*)native_installHook},
    {"nativeLog", "(Ljava/lang/String;)V", (void*)native_log},
    {"nativeCallNext", "(J)Z", (void*)native_callNext},
    {"nativeGetDataDir", "()Ljava/lang/String;", (void*)native_getDataDir},
    {"readU8", "(J)B", (void*)native_readU8},
    {"readU32", "(J)I", (void*)native_readU32},
    {"readU64", "(J)J", (void*)native_readU64},
    {"writeU64", "(JJ)V", (void*)native_writeU64},
    {"writeBytes", "(J[B)V", (void*)native_writeBytes},
};


bool register_jni_natives(JavaVM *vm, const orca_host_api_v1 *host, JvmErrorSink sink, std::string &out_error) {
    g_host = host;
    g_sink = std::move(sink);
    JNIEnv *env = attach_current_thread_daemon(vm);
    if (!env) { out_error = "AttachCurrentThreadAsDaemon failed"; return false; }
    jclass bridge = env->FindClass("org/orcaslicer/plugin/v1/NativeBridge");
    if (!bridge) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        // Tests may lack NativeBridge in the system classpath; that alone must not fail JVM setup.
        std::cout << "[DIAG] register_jni_natives: system loader has NO NativeBridge (FindClass returned null) -> will not register system natives" << std::endl;
        return true;
    }
    std::cout << "[DIAG] register_jni_natives: system loader HAS NativeBridge class=" << bridge << " loader=" << class_loader_string_diag(env, bridge) << std::endl;
    if (env->RegisterNatives(bridge, g_methods, (int)(sizeof(g_methods)/sizeof(g_methods[0]))) != JNI_OK) {
        std::string ex = "";
        if (env->ExceptionCheck()) {
            jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
            ex = throwable_to_string(env, thr) + " | " + throwable_stack_trace(env, thr).substr(0, 600);
            env->DeleteLocalRef(thr);
        }
        env->DeleteLocalRef(bridge);
        out_error = "RegisterNatives failed for NativeBridge: " + ex;
        std::cout << "[DIAG] register_jni_natives RegisterNatives FAILED: " << ex << std::endl;
        return false;
    }
    std::cout << "[DIAG] register_jni_natives RegisterNatives OK for system loader" << std::endl;
    env->DeleteLocalRef(bridge);
    return true;
}

bool register_natives_for_loader(JNIEnv *env, jobject loader, std::string &out_error) {
    if (!env || !loader) { out_error = "null env or loader"; return false; }
    std::cout << "[DIAG] register_natives_for_loader: loader=" << loader_to_string_diag(env, loader) << " loader_ptr=" << loader << std::endl;
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::cout << "[DIAG] pending exception before register_natives_for_loader cleared: " << throwable_to_string(env, thr) << std::endl;
        env->DeleteLocalRef(thr);
    }
    jclass classCls = env->FindClass("java/lang/Class");
    if (!classCls) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        out_error = "java/lang/Class not found";
        return false;
    }
    jmethodID forName = env->GetStaticMethodID(classCls, "forName", "(Ljava/lang/String;ZLjava/lang/ClassLoader;)Ljava/lang/Class;");
    if (!forName) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(classCls);
        out_error = "Class.forName not found";
        return false;
    }
    jstring jName = env->NewStringUTF("org.orcaslicer.plugin.v1.NativeBridge");
    jclass bridge = (jclass)env->CallStaticObjectMethod(classCls, forName, jName, JNI_TRUE, loader);
    env->DeleteLocalRef(jName);
    if (env->ExceptionCheck()) {
        jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
        std::cout << "[DIAG] Class.forName threw: " << throwable_to_string(env, thr) << std::endl;
        env->DeleteLocalRef(thr);
        bridge = nullptr;
    }
    if (!bridge) {
        std::cout << "[DIAG] Class.forName returned null, trying loader.loadClass fallback" << std::endl;
        jclass loaderCls = env->GetObjectClass(loader);
        if (loaderCls) {
            jmethodID loadClassMid = env->GetMethodID(loaderCls, "loadClass", "(Ljava/lang/String;)Ljava/lang/Class;");
            if (loadClassMid) {
                jstring jName2 = env->NewStringUTF("org.orcaslicer.plugin.v1.NativeBridge");
                bridge = (jclass)env->CallObjectMethod(loader, loadClassMid, jName2);
                env->DeleteLocalRef(jName2);
                if (env->ExceptionCheck()) {
                    jthrowable thr2 = env->ExceptionOccurred(); env->ExceptionClear();
                    std::cout << "[DIAG] loader.loadClass threw: " << throwable_to_string(env, thr2) << std::endl;
                    env->DeleteLocalRef(thr2);
                    bridge = nullptr;
                } else {
                    std::cout << "[DIAG] loader.loadClass returned bridge=" << bridge << std::endl;
                }
            } else {
                std::cout << "[DIAG] loadClass method not found" << std::endl;
                if (env->ExceptionCheck()) env->ExceptionClear();
            }
            env->DeleteLocalRef(loaderCls);
        }
    } else {
        std::cout << "[DIAG] Class.forName succeeded: bridge=" << bridge << " bridge.loader=" << class_loader_string_diag(env, bridge) << std::endl;
    }
    if (!bridge) {
        if (env->ExceptionCheck()) env->ExceptionClear();
        env->DeleteLocalRef(classCls);
        out_error = "NativeBridge not found in plugin loader (both forName and loadClass failed)";
        std::cout << "[DIAG] register_natives_for_loader: bridge not found in plugin loader" << std::endl;
        return false;
    }
    {
        jclass sysBridge = env->FindClass("org/orcaslicer/plugin/v1/NativeBridge");
        if (sysBridge) {
            std::cout << "[DIAG] system FindClass also found bridge=" << sysBridge << " sys.loader=" << class_loader_string_diag(env, sysBridge)
                      << " same As plugin? " << (env->IsSameObject(bridge, sysBridge) ? "YES" : "NO") << std::endl;
            env->DeleteLocalRef(sysBridge);
            if (env->ExceptionCheck()) env->ExceptionClear();
        } else {
            if (env->ExceptionCheck()) env->ExceptionClear();
            std::cout << "[DIAG] system FindClass found NO bridge (expected: NativeBridge only in plugin JAR, not system classpath)" << std::endl;
        }
    }
    std::cout << "[DIAG] attempting RegisterNatives for plugin bridge=" << bridge << " loader=" << class_loader_string_diag(env, bridge) << " methods=" << (sizeof(g_methods)/sizeof(g_methods[0])) << std::endl;
    jint rc = env->RegisterNatives(bridge, g_methods, (int)(sizeof(g_methods)/sizeof(g_methods[0])));
    if (rc != JNI_OK) {
        std::string ex = "";
        if (env->ExceptionCheck()) {
            jthrowable thr = env->ExceptionOccurred(); env->ExceptionClear();
            ex = throwable_to_string(env, thr) + " | " + throwable_stack_trace(env, thr).substr(0, 800);
            env->DeleteLocalRef(thr);
        } else {
            ex = "no pending exception, but RegisterNatives returned " + std::to_string(rc);
        }
        env->DeleteLocalRef(bridge);
        env->DeleteLocalRef(classCls);
        out_error = "RegisterNatives failed for plugin NativeBridge: " + ex;
        std::cout << "[DIAG] RegisterNatives FAILED for plugin NativeBridge: " << ex << std::endl;
        return false;
    }
    std::cout << "[DIAG] RegisterNatives SUCCESS for plugin loader's NativeBridge" << std::endl;
    env->DeleteLocalRef(bridge);
    env->DeleteLocalRef(classCls);
    return true;
}

void remove_all_hooks_for_plugin(const std::string &plugin_id) {
    std::vector<std::string> toRemove;
    {
        std::lock_guard<std::mutex> lk(g_hookMu);
        for (auto &kv : g_hooks) if (kv.second.plugin_id == plugin_id) toRemove.push_back(kv.first);
    }
    for (auto &key : toRemove) {
        HookRecord rec;
        {
            std::lock_guard<std::mutex> lk(g_hookMu);
            auto it = g_hooks.find(key);
            if (it == g_hooks.end()) continue;
            rec = it->second;
        }
        if (rec.handle && g_host && g_host->remove_hook) g_host->remove_hook(rec.handle);
        safe_delete_global_ref(rec.vm, rec.globalCallback);
        {
            std::lock_guard<std::mutex> lk(g_hookMu);
            g_hooks.erase(key);
        }
    }
}

}}} // namespace Slic3r::Plugin::Jvm

#else // !ORCA_JVM_AVAILABLE
namespace Slic3r { namespace Plugin { namespace Jvm {
bool register_jni_natives(JavaVM*, const orca_host_api_v1*, JvmErrorSink, std::string &out_error) { out_error = "JVM support not compiled (ORCA_ENABLE_JVM_PLUGINS=OFF)"; return false; }
JNIEnv* attach_current_thread_daemon(JavaVM*) { return nullptr; }
void detach_current_thread_if_attached(JavaVM*) {}
std::string throwable_to_string(JNIEnv*, jthrowable) { return {}; }
std::string throwable_stack_trace(JNIEnv*, jthrowable) { return {}; }
void safe_delete_global_ref(JavaVM*, jobject) {}
void safe_release_classloader(JavaVM*, jobject) {}
bool is_jvm_hook_disabled(const std::string&, const std::string&) { return false; }
void disable_jvm_hook_for_session(const std::string&, const std::string&) {}
void clear_jvm_hook_disables() {}
const orca_host_api_v1* host_table() { return nullptr; }
void set_host_table(const orca_host_api_v1*) {}
void set_loading_plugin_id(const std::string&) {}
std::string loading_plugin_id() { return {}; }
void set_dispatch_next(orca_hook_status_t (*)(orca_cpu_context_t*) noexcept) {}
orca_hook_status_t (*dispatch_next())(orca_cpu_context_t*) noexcept { return nullptr; }
void remove_all_hooks_for_plugin(const std::string&) {}
}}} // namespace Slic3r::Plugin::Jvm
#endif
