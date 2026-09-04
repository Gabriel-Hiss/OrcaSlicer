package org.orcaslicer.plugin.v1;

import java.lang.annotation.*;

@Retention(RetentionPolicy.SOURCE)
@Target(ElementType.METHOD)
public @interface Replace {
    String target();
    HookPoint point() default HookPoint.ENTRY;
    int priority() default 1000;
    String id() default "";
}
