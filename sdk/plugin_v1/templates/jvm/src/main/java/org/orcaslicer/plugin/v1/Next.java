package org.orcaslicer.plugin.v1;

/**
 * Handle for calling next/original in a @Replace hook. Exactly-once semantics enforced by host.
 */
public final class Next<R> {
    private final long ctxPtr;
    private boolean called = false;
    public Next(long ctxPtr) { this.ctxPtr = ctxPtr; }
    public R call() {
        if (called) throw new IllegalStateException("next() already called — exactly once");
        called = true;
        boolean ok = NativeBridge.nativeCallNext(ctxPtr);
        if (!ok) throw new IllegalStateException("call_next failed");
        @SuppressWarnings("unchecked")
        R r = null;
        return r;
    }
    public boolean wasCalled() { return called; }
    /**
     * Re-reads the RAX field of the CPU context snapshot. Whether this reflects
     * post-call state depends on host write-back; currently the host does not
     * write the original's return value back into the context, so after
     * {@link #call()} this still observes the entry snapshot. Real returns are
     * observed via AFTER hooks at {@code HookPoint.RETURN}, whose context is
     * captured post-execution.
     */
    public long result() { return NativeBridge.readU64(ctxPtr + 8); }
}
