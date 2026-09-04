package org.orcaslicer.plugin.v1;

/**
 * Typed hook context passed to @Before/@After/@Replace callbacks generated from manifest.
 * For REPLACE, {@link Next} can be called at most once and is enforced by the host chain.
 */
public final class HookContext<R> {
    private R result;
    private boolean cancelled;
    private final CpuContext raw;
    private final HookTarget target;

    public HookContext(CpuContext raw, HookTarget target) {
        this.raw = raw;
        this.target = target;
    }
    public CpuContext raw() { return raw; }
    public HookTarget target() { return target; }
    public void cancel(R value) { this.cancelled = true; this.result = value; }
    public boolean isCancelled() { return cancelled; }
    public R result() { return result; }
    public void setResult(R v) { this.result = v; }
}
