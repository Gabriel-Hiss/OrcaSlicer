package org.orcaslicer.plugin.v1.examples

import org.orcaslicer.plugin.v1.*

/**
 * Kotlin example — same target as Java/C++/Rust, fat JAR includes stdlib.
 * Bytecode target 25, Kotlin 2.4.0. DSL generates the same Java/JNI registry.
 */
@Hook(priority = 1000)
class ExamplePluginKt {
    companion object {
        @JvmStatic fun orcaRegister() {
            // Invoked via Class.forName/entry_class.orcaRegister()
        }

        @JvmStatic @Before(target = "Slic3r::CLI::print_help", point = HookPoint.ENTRY, priority = 1000, id = "ktexample.before")
        fun beforePrintHelp(ctx: HookContext<Void>) {
        }

        @JvmStatic @After(target = "Slic3r::CLI::print_help", point = HookPoint.RETURN, priority = 1000, id = "ktexample.after")
        fun afterPrintHelp(ctx: HookContext<Void>) {
        }
    }
}

// DSL alternative — also validated by processor, same JNI table:
val kotlinHooks = hooks {
    before("Slic3r::CLI::print_help", HookPoint.ENTRY, priority = 1000) { ctx ->
    }
    after("Slic3r::CLI::print_help", HookPoint.RETURN) { ctx ->
    }
}
