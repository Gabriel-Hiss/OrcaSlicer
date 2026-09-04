package org.orcaslicer.plugin.v1;

import java.lang.annotation.*;

/**
 * Mid-hook / offset / vtable / import hook refinement.
 * Used together with @Before/@After/@Replace or alone for raw hooks.
 */
@Retention(RetentionPolicy.SOURCE)
@Target(ElementType.METHOD)
public @interface At {
    /** INVOKE requires ordinal, OFFSET requires rva, VTABLE requires index, IAT/GOT requires module+name. */
    HookPoint value();
    /** For OFFSET: rva inside target range at instruction boundary (validated). */
    long rva() default -1;
    /** For INVOKE: callee target + ordinal. */
    String callee() default "";
    int ordinal() default -1;
    /** For VTABLE: slot index. */
    int vtableIndex() default -1;
    boolean perInstance() default false;
    /** For IAT/GOT: module + import name. */
    String module() default "";
    String importName() default "";
}
