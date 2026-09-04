package org.orcaslicer.plugin.v1;

import java.lang.annotation.*;

/**
 * Marks a class as a hook container. The annotation processor validates
 * every @Before/@After/@Replace/@At declaration against the manifest
 * (build_id + symbol table) and generates a registry that is invoked
 * without reflection at load time.
 */
@Retention(RetentionPolicy.SOURCE)
@Target(ElementType.TYPE)
public @interface Hook {
    /** Optional priority override for all hooks in this class (default 1000). */
    int priority() default 1000;
}
