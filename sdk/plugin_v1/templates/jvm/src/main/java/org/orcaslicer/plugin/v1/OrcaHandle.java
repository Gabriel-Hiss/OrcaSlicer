package org.orcaslicer.plugin.v1;

/**
 * Borrowed, non-owning handle to a native Orca object.
 * Generated aggregates that are trivially-copyable are inlined;
 * classes/unions/STL and other non-trivial objects are exposed as
 * typed borrowed handles, never as inferred owners. The address is
 * valid only for the duration of the hook callback.
 */
public final class OrcaHandle<T> {
    private final long nativeAddress;
    private final String typeName;

    OrcaHandle(long addr, String typeName) {
        this.nativeAddress = addr;
        this.typeName = typeName;
    }

    public long address() { return nativeAddress; }
    public String typeName() { return typeName; }
    public boolean isNull() { return nativeAddress == 0; }

    @Override public String toString() {
        return "OrcaHandle<" + typeName + ">(0x" + Long.toHexString(nativeAddress) + ")";
    }
}
