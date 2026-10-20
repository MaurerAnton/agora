package androidx.compose.runtime

/**
 * Stand-local stand-in for the Compose stability marker used by StreamingTextDelta.
 *
 * ChatMessage.kt cannot compile host-side because it is annotated with @Immutable, so
 * extract_model_types.py copies that declaration out verbatim and this package supplies the
 * annotation. @Immutable is a pure compile-time marker: it has no runtime behaviour and no effect
 * on MessageSegment's constructor, copy() or data-class toString(), which is what the policy pins.
 */
@Target(
    AnnotationTarget.CLASS,
    AnnotationTarget.FUNCTION,
    AnnotationTarget.PROPERTY,
    AnnotationTarget.TYPE,
)
@Retention(AnnotationRetention.BINARY)
annotation class Immutable
