package org.orcaslicer.plugin.v1;

import java.lang.annotation.*;

/**
 * Executed high→low before the target. May mutate args or cancel.
 * Signature is validated against the typed manifest binding; raw escape
 * uses {@link RawHook} / {@link CpuContext} when typed binding is unavailable.
 */
@Retention(RetentionPolicy.SOURCE)
@Target(ElementType.METHOD)
public @interface Before {
    /** Symbol id or qualified name from manifest (e.g. "Slic3r::CLI::print_help"). */
    String target();
    /** Hook point; defaults to ENTRY. */
    HookPoint point() default HookPoint.ENTRY;
    /** Priority; higher runs first. Default 1000. */
    int priority() default 1000;
    /** Stable hook id for diagnostics (auto-derived if empty). */
    String id() default "";
}
