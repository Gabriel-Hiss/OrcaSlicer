package org.orcaslicer.plugin.v1

/**
 * Kotlin DSL — generates the same Java/JNI registry; no second runtime.
 * Bytecode target 25, Kotlin 2.4.0, nullability explicit.
 *
 * Usage:
 * ```
 * class MyPlugin {
 *   val demoHooks = hooks {
 *     before("Slic3r::CLI::print_help", HookPoint.ENTRY, priority = 1500, id = "k.before.high") { ctx ->
 *       DemoLog.log("com.orca.kotlin-example", "k.before.high", "Slic3r::CLI::print_help", ctx)
 *     }
 *     after("Slic3r::CLI::print_help", HookPoint.RETURN, priority = 1000, id = "k.after") { ctx ->
 *       DemoLog.log("com.orca.kotlin-example", "k.after", "Slic3r::CLI::print_help", ctx)
 *     }
 *     replace("Slic3r::CLI::print_help") { next, ctx ->
 *       DemoLog.log("com.orca.kotlin-example", "k.replace.enter", "Slic3r::CLI::print_help", ctx)
 *       next.call()
 *       DemoLog.log("com.orca.kotlin-example", "k.replace.exit", "Slic3r::CLI::print_help", ctx)
 *     }
 *   }
 *   fun register() { demoHooks.installAll() }
 * }
 * ```
 */

@DslMarker
annotation class OrcaDsl

@OrcaDsl
class HooksBuilder {
    val entries = mutableListOf<HookEntry>()
    fun before(target: String, point: HookPoint = HookPoint.ENTRY, priority: Int = 1000, id: String = "", handler: (HookContext<*>) -> Unit) {
        entries += HookEntry(target, point, HookKind.BEFORE, priority, id.ifEmpty { "before:$target" }, handler as Any)
    }
    fun after(target: String, point: HookPoint = HookPoint.RETURN, priority: Int = 1000, id: String = "", handler: (HookContext<*>) -> Unit) {
        entries += HookEntry(target, point, HookKind.AFTER, priority, id.ifEmpty { "after:$target" }, handler as Any)
    }
    fun replace(target: String, point: HookPoint = HookPoint.ENTRY, priority: Int = 1000, id: String = "", handler: (Next<*>, HookContext<*>) -> Unit) {
        entries += HookEntry(target, point, HookKind.REPLACE, priority, id.ifEmpty { "replace:$target" }, handler as Any, isReplace = true)
    }
    fun raw(target: String, point: HookPoint, priority: Int = 1000, id: String = "", handler: (RawHook) -> Unit) {
        entries += HookEntry(target, point, HookKind.BEFORE, priority, id.ifEmpty { "raw:$target" }, handler as Any)
    }
    enum class HookKind { BEFORE, AFTER, REPLACE }
    data class HookEntry(
        val target: String,
        val point: HookPoint,
        val kind: HookKind,
        val priority: Int,
        val id: String,
        val handler: Any,
        val isReplace: Boolean = false
    )
}

fun hooks(block: HooksBuilder.() -> Unit): List<HooksBuilder.HookEntry> =
    HooksBuilder().apply(block).entries

/**
 * Install all hooks from a DSL list via NativeBridge.
 * Must be called from orcaRegister() while loading_plugin_id TLS is set.
 */
fun List<HooksBuilder.HookEntry>.installAll(pluginId: String = "com.orca.kotlin-example") {
    for (e in this) {
        val cb: Any = if (e.isReplace) {
            ReplaceCallback(e)
        } else {
            HookCallback(e)
        }
        val pointOrd = e.point.ordinal
        val kindOrd = when (e.kind) {
            HooksBuilder.HookKind.BEFORE -> 0
            HooksBuilder.HookKind.AFTER -> 1
            HooksBuilder.HookKind.REPLACE -> 2
        }
        val rc = NativeBridge.nativeInstallHook(e.id + "|" + e.target, 0L, pointOrd, kindOrd, e.priority, cb)
        DemoLog.installReceipt(pluginId, e.id, e.target, pointOrd, kindOrd, e.priority, rc)
        if (rc != 0) {
            NativeBridge.log("kotlin DSL install failed id=${e.id} rc=$rc")
        }
    }
}

private class HookCallback(private val entry: HooksBuilder.HookEntry) {
    @Suppress("unused")
    fun handle(ctxPtr: Long, outPtr: Long) {
        try {
            val cpu = readCpuContext(ctxPtr)
            val target = HookTarget(0, 0L, entry.target)
            val hookCtx = HookContext<Any>(cpu, target)
            @Suppress("UNCHECKED_CAST")
            val h = entry.handler as (HookContext<*>) -> Unit
            h(hookCtx)
        } catch (t: Throwable) {
            NativeBridge.log("kotlin hook ${entry.id} threw: ${t.message}")
            throw t
        }
    }
    @Suppress("unused")
    fun invoke(ctxPtr: Long, outPtr: Long) = handle(ctxPtr, outPtr)
    @Suppress("unused")
    fun onHook(ctxPtr: Long, outPtr: Long) = handle(ctxPtr, outPtr)
}

private class ReplaceCallback(private val entry: HooksBuilder.HookEntry) {
    @Suppress("unused")
    fun handle(ctxPtr: Long, outPtr: Long) {
        try {
            val cpu = readCpuContext(ctxPtr)
            val target = HookTarget(0, 0L, entry.target)
            val hookCtx = HookContext<Any>(cpu, target)
            val next = Next<Any>(ctxPtr)
            @Suppress("UNCHECKED_CAST")
            val h = entry.handler as (Next<*>, HookContext<*>) -> Unit
            h(next as Next<*>, hookCtx as HookContext<*>)
        } catch (t: Throwable) {
            NativeBridge.log("kotlin replace ${entry.id} threw: ${t.message}")
            throw t
        }
    }
    @Suppress("unused")
    fun invoke(ctxPtr: Long, outPtr: Long) = handle(ctxPtr, outPtr)
    @Suppress("unused")
    fun onHook(ctxPtr: Long, outPtr: Long) = handle(ctxPtr, outPtr)
}

private fun readCpuContext(ptr: Long): CpuContext {
    if (ptr == 0L) return CpuContext()
    val c = CpuContext()
    try {
        // orca_cpu_context_t layout: size(0), version(4), rax(8), rbx(16), rcx(24), rdx(32), rsi(40), rdi(48), rbp(56), rsp(64), r8(72), r9(80), r10(88), r11(96), r12(104), r13(112), r14(120), r15(128), rip(136), rflags(144), xmm(152)
        c.rax = NativeBridge.readU64(ptr + 8)
        c.rbx = NativeBridge.readU64(ptr + 16)
        c.rcx = NativeBridge.readU64(ptr + 24)
        c.rdx = NativeBridge.readU64(ptr + 32)
        c.rsi = NativeBridge.readU64(ptr + 40)
        c.rdi = NativeBridge.readU64(ptr + 48)
        c.rbp = NativeBridge.readU64(ptr + 56)
        c.rsp = NativeBridge.readU64(ptr + 64)
        c.r8 = NativeBridge.readU64(ptr + 72)
        c.r9 = NativeBridge.readU64(ptr + 80)
        c.r10 = NativeBridge.readU64(ptr + 88)
        c.r11 = NativeBridge.readU64(ptr + 96)
        c.r12 = NativeBridge.readU64(ptr + 104)
        c.r13 = NativeBridge.readU64(ptr + 112)
        c.r14 = NativeBridge.readU64(ptr + 120)
        c.r15 = NativeBridge.readU64(ptr + 128)
        c.rip = NativeBridge.readU64(ptr + 136)
        c.rflags = NativeBridge.readU64(ptr + 144)
        for (i in 0 until 32) {
            try { c.xmm[i] = NativeBridge.readU64(ptr + 152 + i * 8) } catch (_: Throwable) { c.xmm[i] = 0L }
        }
    } catch (_: Throwable) {
    }
    return c
}

/**
 * Demo log helper — writes one line per event to <data_dir>/orca_plugins/demo-logs/kotlin-demo.log
 * Uses APPDATA fallback and creates directories.
 */
object DemoLog {
    private const val PLUGIN_ID_CONST = "com.orca.kotlin-example"
    private const val LOG_NAME = "kotlin-demo.log"
    @Volatile private var initialized = false
    private var seq = 0

    fun init(pluginId: String = PLUGIN_ID_CONST) {
        try {
            val f = logFile()
            f.parentFile?.mkdirs()
            synchronized(this) { seq = 0 }
            if (!initialized) {
                val hdr = "=== kotlin-demo init plugin=$pluginId ts=${System.currentTimeMillis()} ===\n"
                f.appendText(hdr)
                initialized = true
            }
        } catch (_: Throwable) {}
    }

    fun log(pluginId: String, hookId: String, target: String, ctx: HookContext<*>) {
        log(pluginId, hookId, target, "event", ctx.raw())
    }

    fun log(pluginId: String, hookId: String, target: String, phase: String, ctx: HookContext<*>) {
        log(pluginId, hookId, target, phase, ctx.raw())
    }

    fun log(pluginId: String, hookId: String, target: String, phase: String, cpu: CpuContext) {
        try {
            val f = logFile()
            f.parentFile?.mkdirs()
            val line = buildString {
                val n = synchronized(this@DemoLog) { ++seq }
                append("seq=").append(n).append(" | ")
                append(pluginId); append(" | ")
                append("hook=").append(hookId); append(" | ")
                append("target=").append(target); append(" | ")
                append("phase=").append(phase); append(" | ")
                append("rax=0x").append(cpu.rax.toString(16)); append(" ")
                append("rcx=0x").append(cpu.rcx.toString(16)); append(" ")
                append("rdx=0x").append(cpu.rdx.toString(16)); append(" ")
                append("r8=0x").append(cpu.r8.toString(16)); append(" ")
                append("r9=0x").append(cpu.r9.toString(16)); append(" ")
                append("rip=0x").append(cpu.rip.toString(16)); append(" ")
                append("rsp=0x").append(cpu.rsp.toString(16))
            }
            synchronized(this) { f.appendText(line + "\n") }
            try { NativeBridge.log(line) } catch (_: Throwable) {}
        } catch (_: Throwable) {}
    }
    fun logReturn(pluginId: String, hookId: String, target: String, phase: String, cpu: CpuContext, ret: Long) {
        try {
            val f = logFile()
            f.parentFile?.mkdirs()
            val line = buildString {
                val n = synchronized(this@DemoLog) { ++seq }
                append("seq=").append(n).append(" | ")
                append(pluginId); append(" | ")
                append("hook=").append(hookId); append(" | ")
                append("target=").append(target); append(" | ")
                append("phase=").append(phase); append(" | ")
                append("rax=0x").append(cpu.rax.toString(16)); append(" ")
                append("rcx=0x").append(cpu.rcx.toString(16)); append(" ")
                append("rdx=0x").append(cpu.rdx.toString(16)); append(" ")
                append("r8=0x").append(cpu.r8.toString(16)); append(" ")
                append("r9=0x").append(cpu.r9.toString(16)); append(" ")
                append("rip=0x").append(cpu.rip.toString(16)); append(" ")
                append("rsp=0x").append(cpu.rsp.toString(16)); append(" ")
                append("ret=0x").append(ret.toString(16))
            }
            synchronized(this) { f.appendText(line + "\n") }
            try { NativeBridge.log(line) } catch (_: Throwable) {}
        } catch (_: Throwable) {}
    }
    fun installReceipt(pluginId: String, hookId: String, target: String, point: Int, kind: Int, priority: Int, rc: Int) {
        try {
            val f = logFile()
            f.parentFile?.mkdirs()
            val n = synchronized(this) { ++seq }
            val line = "seq=$n | $pluginId | hook=$hookId | target=$target | phase=install point=$point kind=$kind priority=$priority rc=$rc"
            synchronized(this) { f.appendText(line + "\n") }
            try { NativeBridge.log(line) } catch (_: Throwable) {}
        } catch (_: Throwable) {}
    }

    private fun logFile(): java.io.File {
        val base = resolveDataDir()
        return java.io.File(base, "orca_plugins/demo-logs/$LOG_NAME")
    }

    private fun resolveDataDir(): java.io.File {
        // 0) Native host data_dir (most reliable, respects --datadir)
        try {
            val d = NativeBridge.getDataDir()
            if (!d.isNullOrBlank()) {
                val f = java.io.File(d)
                if (f.exists() || d.length > 5) return f
            }
        } catch (_: Throwable) {}
        try {
            val cs = DemoLog::class.java.protectionDomain?.codeSource?.location
            if (cs != null) {
                var jarFile = java.io.File(cs.toURI())
                var dir: java.io.File? = jarFile
                if (dir != null && dir.isFile) dir = dir.parentFile
                while (dir != null) {
                    if (dir.name == "orca_plugins") {
                        val dataDir = dir.parentFile
                        if (dataDir != null && dataDir.exists()) return dataDir
                        if (dataDir != null) return dataDir
                    }
                    dir = dir.parentFile
                    if (dir != null && dir.path.length < 10) break
                }
                try {
                    val maybeDataDir = jarFile.parentFile?.parentFile?.parentFile
                    if (maybeDataDir != null && maybeDataDir.name != "or" && maybeDataDir.exists()) {
                        if (java.io.File(maybeDataDir, "orca_plugins").exists()) return maybeDataDir
                    }
                } catch (_: Throwable) {}
            }
        } catch (_: Throwable) {}
        val candidates = mutableListOf<String>()
        try { System.getProperty("orca.data_dir")?.let { if (it.isNotBlank()) candidates.add(it) } } catch (_: Throwable) {}
        try { System.getProperty("slic3r.data_dir")?.let { if (it.isNotBlank()) candidates.add(it) } } catch (_: Throwable) {}
        try { System.getenv("ORCA_DATA_DIR")?.let { if (it.isNotBlank()) candidates.add(it) } } catch (_: Throwable) {}
        try { System.getenv("SLIC3R_DATA_DIR")?.let { if (it.isNotBlank()) candidates.add(it) } } catch (_: Throwable) {}
        try { System.getenv("APPDATA")?.let { candidates.add("$it/OrcaSlicer") } } catch (_: Throwable) {}
        try { System.getenv("LOCALAPPDATA")?.let { candidates.add("$it/OrcaSlicer") } } catch (_: Throwable) {}
        try { System.getProperty("user.home")?.let { candidates.add("$it/AppData/Roaming/OrcaSlicer") } } catch (_: Throwable) {}
        candidates.add("C:/Users/User/AppData/Roaming/OrcaSlicer")
        for (c in candidates) {
            if (c.isBlank()) continue
            val f = java.io.File(c)
            if (f.exists() && f.isDirectory) return f
        }
        return java.io.File(candidates.firstOrNull() ?: "C:/Users/User/AppData/Roaming/OrcaSlicer")
    }
}

// Annotations are available directly via org.orcaslicer.plugin.v1.Hook etc; no re-export needed here.
