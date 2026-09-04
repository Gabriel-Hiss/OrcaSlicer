package org.orcaslicer.plugin.v1;

/**
 * Raw CPU context — all GPRs, RIP/RSP/RFLAGS, XMM0-15.
 * Mirrors orca_cpu_context_t from sdk/plugin_v1/abi/orca_hook_api.h
 * without duplicating layout; field names match the C ABI.
 * Raw hooks receive this for machine-level access.
 */
public final class CpuContext {
    public long rax, rbx, rcx, rdx, rsi, rdi, rbp, rsp, rip, rflags;
    public long r8, r9, r10, r11, r12, r13, r14, r15;
    public long[] xmm = new long[32]; // 16 * (low,high)

    // Helpers delegated to NativeBridge via JNI (read/write with explicit size,
    // protect and flush). No helpers beyond escape; domain logic stays in Orca.
    public byte readU8(long addr) { return NativeBridge.readU8(addr); }
    public int  readU32(long addr) { return NativeBridge.readU32(addr); }
    public long readU64(long addr) { return NativeBridge.readU64(addr); }
    public void writeU64(long addr, long v) { NativeBridge.writeU64(addr, v); }
    public void writeBytes(long addr, byte[] data) { NativeBridge.writeBytes(addr, data); }

    @Override public String toString() {
        return String.format("CpuContext{rip=0x%x rsp=0x%x rax=0x%x}", rip, rsp, rax);
    }
}
