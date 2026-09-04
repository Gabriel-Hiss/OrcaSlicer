package org.orcaslicer.plugin.v1;

/**
 * JNI bridge — natives are registered (JNI_VERSION_21 max; Java 25 validated via release file, not JNI level) against orca_host_api_v1 at VM init
 * (no System.loadLibrary, no reflection scan). Calls flow through the same
 * host table used by native plugins: resolve, protect, flush, call_next.
 */
public final class NativeBridge {
    private NativeBridge() {}

    // Loaded implicitly when the host registers natives; no static System.loadLibrary.

    public static native int nativeInstallHook(String hookId, long targetRvaOrId, int point, int kind, int priority, Object callback);
    public static native void nativeLog(String msg);
    public static native boolean nativeCallNext(long ctxPtr);
    public static native String nativeGetDataDir();

    // Raw memory helpers (explicit size, protect + flush handled inside host)
    public static native byte readU8(long addr);
    public static native int  readU32(long addr);
    public static native long readU64(long addr);
    public static native void writeU64(long addr, long value);
    public static native void writeBytes(long addr, byte[] data);

    public static void log(String s) { nativeLog(s); }
    public static String getDataDir() {
        try { return nativeGetDataDir(); } catch (Throwable t) { return null; }
    }
}
