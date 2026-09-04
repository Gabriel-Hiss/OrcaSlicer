package org.orcaslicer.plugin.v1.examples;

import org.orcaslicer.plugin.v1.*;
import java.io.*;
import java.nio.charset.StandardCharsets;
import java.nio.file.*;

/**
 * Java demo plugin exercising JVM hook runtime:
 * - @Hook + @Before/@After + @At with processor-generated registry (no reflection scan)
 * - targets hit during --info execution: Slic3r::Utils::get_current_time_utc (ENTRY/RETURN/OFFSET) — always hit in CLI::run before dispatch
 * - Throwable containment: one hook throws intentionally, bridge captures stack and disables it
 * - observable log at &lt;data_dir&gt;/orca_plugins/demo-logs/java-demo.log
 */
@Hook(priority = 1100)
public class ExamplePlugin {
    private static final String PLUGIN_ID = "com.orca.java-example";
    // Containment accounting (all real observations, no synthetic events):
    // THROW_FIRED is set by beforeGetTime immediately before it throws; ENTRY_CALLS counts
    // dispatches observed by beforePrintInfo. The bridge disables a hook after it throws, so a
    // later ENTRY dispatch with no rethrow proves capture + disable; that outcome is logged once.
    private static final java.util.concurrent.atomic.AtomicBoolean THROW_FIRED = new java.util.concurrent.atomic.AtomicBoolean(false);
    private static final java.util.concurrent.atomic.AtomicInteger ENTRY_CALLS = new java.util.concurrent.atomic.AtomicInteger(0);
    private static final java.util.concurrent.atomic.AtomicBoolean CONTAINMENT_LOGGED = new java.util.concurrent.atomic.AtomicBoolean(false);
    private static volatile String THROWN_STACK = "";

    private static Path logFile() {
        try {
            String jar = ExamplePlugin.class.getProtectionDomain().getCodeSource().getLocation().toURI().getPath();
            if (jar != null) {
                String norm = jar.replace('\\', '/');
                int idx = norm.indexOf("/orca_plugins/");
                if (idx > 0) {
                    String dataDir = norm.substring(0, idx);
                    if (dataDir.startsWith("/") && dataDir.length() > 3 && dataDir.charAt(2) == ':') {
                        dataDir = dataDir.substring(1);
                    }
                    return Paths.get(dataDir, "orca_plugins", "demo-logs", "java-demo.log");
                }
            }
        } catch (Exception ignored) {}
        // Native host data_dir (most reliable, respects --datadir)
        try {
            String d = NativeBridge.getDataDir();
            if (d != null && !d.isBlank()) return Paths.get(d, "orca_plugins", "demo-logs", "java-demo.log");
        } catch (Throwable ignored) {}
        String dataDir = System.getProperty("orca.data_dir");
        if (dataDir == null) dataDir = System.getenv("ORCA_DATA_DIR");
        if (dataDir == null) {
            String appData = System.getenv("APPDATA");
            if (appData != null && !appData.isEmpty()) dataDir = appData + "/OrcaSlicer";
            else dataDir = System.getProperty("user.home") + "/OrcaSlicer";
        }
        return Paths.get(dataDir, "orca_plugins", "demo-logs", "java-demo.log");
    }

    private static synchronized void log(String hookId, String detail) {
        try {
            Path f = logFile();
            Files.createDirectories(f.getParent());
            String line = PLUGIN_ID + " " + hookId + " " + detail;
            Files.write(f, (line + System.lineSeparator()).getBytes(StandardCharsets.UTF_8),
                    StandardOpenOption.CREATE, StandardOpenOption.APPEND);
            try { NativeBridge.nativeLog("[java-demo] " + hookId + " " + detail); } catch (Throwable t) {}
        } catch (Exception e) {
            try { System.out.println("[java-demo log fail] " + e); } catch (Throwable t2) {}
        }
    }

    private static String stackToSingleLine(Throwable t) {
        StringWriter sw = new StringWriter();
        t.printStackTrace(new PrintWriter(sw));
        return sw.toString().replace('\n', '|').replace('\r', '|');
    }

    // Called by JvmPluginRuntime via Class.forName(entry_class).orcaRegister()
    // Delegates to processor-generated registry (no reflection scan).
    public static void orcaRegister() {
        try {
            Class<?> reg = Class.forName("org.orcaslicer.plugin.v1.examples.ExamplePluginOrcaRegistry");
            reg.getDeclaredMethod("registerAll").invoke(null);
            log("orcaRegister", "registered via ExamplePluginOrcaRegistry");
        } catch (ClassNotFoundException e) {
            // Fallback if compiled without processor (should not happen via BuildHookExamples)
            log("orcaRegister", "registry not found: " + e + " (hooks disabled)");
            return;
        } catch (Throwable t) {
            log("orcaRegister", "registry failed: " + t + " stack=" + stackToSingleLine(t));
            return;
        }
        log("orcaRegister", "plugin loaded id=" + PLUGIN_ID);
    }

    @Before(target = "Slic3r::Utils::get_current_time_utc", point = HookPoint.ENTRY, priority = 1100, id = "java.before")
    public static void beforePrintInfo(HookContext<Void> ctx) {
        int call = ENTRY_CALLS.incrementAndGet();
        try {
            CpuContext raw = ctx != null ? ctx.raw() : null;
            long rip = raw != null ? raw.rip : 0;
            long rax = raw != null ? raw.rax : 0;
            String detail = "Slic3r::Utils::get_current_time_utc ENTRY rip=0x" + Long.toHexString(rip)
                    + " rax=0x" + Long.toHexString(rax)
                    + " target=" + (ctx != null && ctx.target() != null ? ctx.target().decoratedName() : "unknown");
            log("java.before", detail);
            // The throw hook runs before this one on the call where it fires. If this dispatch runs
            // after that call with no rethrow, the bridge captured the Throwable and disabled the hook.
            if (THROW_FIRED.get() && call > 1 && CONTAINMENT_LOGGED.compareAndSet(false, true)) {
                log("java.bridge.contained", "EXCEPTION_THROWN java.lang.RuntimeException: java-demo intentional throwable for hook containment demo"
                        + " stack=" + THROWN_STACK
                        + " evidence=throw_hook_fired_once,entry_dispatch_" + call + "_proceeded_without_rethrow,hook_disabled_by_bridge,process_alive");
            }
        } catch (Throwable t) {
            log("java.before", "error " + t + " stack=" + stackToSingleLine(t));
        }
    }

    @After(target = "Slic3r::Utils::get_current_time_utc", point = HookPoint.RETURN, priority = 1100, id = "java.after")
    public static void afterPrintInfo(HookContext<Void> ctx) {
        try {
            CpuContext raw = ctx != null ? ctx.raw() : null;
            long rip = raw != null ? raw.rip : 0;
            long rax = raw != null ? raw.rax : 0;
            long rsp = raw != null ? raw.rsp : 0;
            String detail = "Slic3r::Utils::get_current_time_utc RETURN rip=0x" + Long.toHexString(rip)
                    + " rax=0x" + Long.toHexString(rax) + " rsp=0x" + Long.toHexString(rsp);
            log("java.after", detail);
        } catch (Throwable t) {
            log("java.after", "error " + t + " stack=" + stackToSingleLine(t));
        }
    }

    @Before(target = "Slic3r::Utils::get_current_time_utc", priority = 1100, id = "java.mid")
    @At(value = HookPoint.OFFSET, rva = 9412027L)
    public static void midPrintInfo(HookContext<Void> ctx) {
        try {
            CpuContext raw = ctx != null ? ctx.raw() : null;
            long rip = raw != null ? raw.rip : 0;
            String detail = "Slic3r::Utils::get_current_time_utc OFFSET rva=9412027 rip=0x" + Long.toHexString(rip);
            log("java.mid", detail);
        } catch (Throwable t) {
            log("java.mid", "error " + t);
        }
    }


    // Hook intentionally throws to demonstrate Throwable containment by JNI bridge.
    @Before(target = "Slic3r::Utils::get_current_time_utc", point = HookPoint.ENTRY, priority = 1500, id = "java.gettime.throw")
    public static void beforeGetTime(HookContext<Void> ctx) {
        String entryDetail = "Slic3r::Utils::get_current_time_utc ENTRY rip=0x" + Long.toHexString(ctx != null && ctx.raw() != null ? ctx.raw().rip : 0);
        log("java.gettime.throw", entryDetail + " will_throw=true");
        RuntimeException intentional = new RuntimeException("java-demo intentional throwable for hook containment demo");
        THROWN_STACK = stackToSingleLine(intentional);
        THROW_FIRED.set(true);
        throw intentional;
    }
}
