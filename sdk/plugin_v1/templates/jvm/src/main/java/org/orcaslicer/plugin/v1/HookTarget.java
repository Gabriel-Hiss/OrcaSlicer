package org.orcaslicer.plugin.v1;

/**
 * Typed hook target handle. Instances are created only by generated
 * bindings (one per symbol in manifest/orca-hooks.json).
 * Encapsulates symbol id + RVA and typed invocation helpers.
 */
public final class HookTarget {
    private final int symbolId;
    private final long rva;
    private final String decoratedName;

    public HookTarget(int symbolId, long rva, String decoratedName) {
        this.symbolId = symbolId;
        this.rva = rva;
        this.decoratedName = decoratedName;
    }

    public int symbolId() { return symbolId; }
    public long rva() { return rva; }
    public String decoratedName() { return decoratedName; }

    @Override public String toString() {
        return "HookTarget{id=" + symbolId + ", rva=0x" + Long.toHexString(rva) + ", " + decoratedName + "}";
    }
}
