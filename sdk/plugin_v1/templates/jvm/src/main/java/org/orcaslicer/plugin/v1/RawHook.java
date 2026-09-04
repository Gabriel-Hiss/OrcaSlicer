package org.orcaslicer.plugin.v1;

/**
 * Escape hatch for raw hooks. All registers, flags, stack pointer,
 * explicit-size read/write, temporary protect and icache flush.
 * Never pretends a non-trivial C++ type is portable.
 */
public final class RawHook {
    private final CpuContext ctx;
    private final HookTarget target;

    RawHook(CpuContext ctx, HookTarget target) {
        this.ctx = ctx;
        this.target = target;
    }
    public CpuContext cpu() { return ctx; }
    public HookTarget target() { return target; }
}
