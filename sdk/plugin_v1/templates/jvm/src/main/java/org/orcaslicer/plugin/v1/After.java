package org.orcaslicer.plugin.v1;

import java.lang.annotation.*;

@Retention(RetentionPolicy.SOURCE)
@Target(ElementType.METHOD)
public @interface After {
    String target();
    HookPoint point() default HookPoint.RETURN;
    int priority() default 1000;
    String id() default "";
}
