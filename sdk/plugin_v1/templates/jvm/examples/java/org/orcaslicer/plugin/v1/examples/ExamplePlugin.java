package org.orcaslicer.plugin.v1.examples;

import org.orcaslicer.plugin.v1.*;

/**
 * Executable example — intercepts the same Orca method as C++/Rust plugins,
 * calls next/original, and writes an observable marker.
 * Generated registry (HookProcessor) calls orcaRegister() without reflection.
 */
@Hook(priority = 1100)
public class ExamplePlugin {

    // Called by JvmPluginRuntime via Class.forName/entry_class.orcaRegister()
    public static void orcaRegister() {
        // Generated OrcaRegistry is produced by HookProcessor; standalone build without processor is not supported.
        NativeBridge.nativeLog("ExamplePlugin orcaRegister");
    }

    @Before(target = "Slic3r::CLI::print_help", point = HookPoint.ENTRY, priority = 1100, id = "example.before")
    public static void beforePrintHelp(HookContext<Void> ctx) {
        // Typed access when typed_binding.available; raw escape otherwise:
        // CpuContext raw = ctx.raw();
        NativeBridge.nativeLog("[java] before Slic3r::CLI::print_help");
    }

    @After(target = "Slic3r::CLI::print_help", point = HookPoint.RETURN, priority = 1100, id = "example.after")
    public static void afterPrintHelp(HookContext<Void> ctx) {
        NativeBridge.nativeLog("[java] after Slic3r::CLI::print_help");
    }

    @Replace(target = "Slic3r::CLI::print_help", priority = 1100, id = "example.replace")
    public static void replacePrintHelp(Next<Void> next, HookContext<Void> ctx) {
        NativeBridge.nativeLog("[java] replace enter");
        next.call();
        NativeBridge.nativeLog("[java] replace leave");
    }
}
