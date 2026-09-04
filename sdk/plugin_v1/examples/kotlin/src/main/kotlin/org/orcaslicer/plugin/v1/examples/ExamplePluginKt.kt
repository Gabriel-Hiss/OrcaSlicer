package org.orcaslicer.plugin.v1.examples

import org.orcaslicer.plugin.v1.HookPoint
import org.orcaslicer.plugin.v1.hooks
import org.orcaslicer.plugin.v1.installAll
import org.orcaslicer.plugin.v1.DemoLog

/**
 * Kotlin demo plugin — real hooks via the `hooks { }` DSL with observable log.
 *
 * Target: Slic3r::Utils::get_current_time_utc, hit on every headless CLI run
 * (called from CLI::run before dispatch), so `orca-slicer.exe --info <stl>`
 * always fires these hooks after plugin installation.
 *
 * - Two before hooks on the same target demonstrate priority ordering: the low
 *   priority hook is deliberately declared first, so the log proves the runtime
 *   runs high priority first rather than following declaration order.
 * - One after hook at RETURN captures the real return value (RAX after the
 *   original ran; its context is captured post-execution).
 * - One replace hook calls next exactly once; the exit line proves next()
 *   returned, i.e. the original ran between enter and exit.
 *
 * Every callback logs the real registers read from orca_cpu_context_t to
 * <data_dir>/orca_plugins/demo-logs/kotlin-demo.log.
 */
private const val PLUGIN_ID = "com.orca.kotlin-example"
private const val TARGET_GET_TIME = "Slic3r::Utils::get_current_time_utc"

private val kotlinDemoHooks = hooks {
    // Intentionally declare low priority first: the runtime must still run high first.
    before(TARGET_GET_TIME, HookPoint.ENTRY, priority = 1000, id = "kotlin.before.low") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.before.low", TARGET_GET_TIME, "before-low-p1000", ctx)
    }
    // High priority before — must appear first in the log despite being declared second.
    before(TARGET_GET_TIME, HookPoint.ENTRY, priority = 1500, id = "kotlin.before.high") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.before.high", TARGET_GET_TIME, "before-high-p1500", ctx)
    }
    // After hook — runs after the original returned, RAX holds the real return value.
    after(TARGET_GET_TIME, HookPoint.RETURN, priority = 1000, id = "kotlin.after") { ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.after", TARGET_GET_TIME, "after-p1000", ctx)
    }
    // Replace hook — wraps the original, must call next exactly once.
    // The exit line proves next() returned (the original ran between enter and exit).
    replace(TARGET_GET_TIME, HookPoint.ENTRY, priority = 900, id = "kotlin.replace") { next, ctx ->
        DemoLog.log(PLUGIN_ID, "kotlin.replace.enter", TARGET_GET_TIME, "replace-enter", ctx)
        next.call() // exactly once; the host enforces at-most-once semantics
        DemoLog.log(PLUGIN_ID, "kotlin.replace.exit", TARGET_GET_TIME, "replace-exit", ctx)
    }
}

class ExamplePluginKt {
    companion object {
        @JvmStatic
        fun orcaRegister() {
            DemoLog.init(PLUGIN_ID)
            kotlinDemoHooks.installAll(PLUGIN_ID)
        }
    }
}
