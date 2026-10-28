package androidx.compose.runtime

/**
 * Stand-local stand-in for the Compose marker annotation on StreamingTextDelta.
 *
 * ChatMessage.kt cannot compile host-side because it is annotated with @Immutable, so
 * agora_extract message-segment copies that declaration out verbatim and points the annotation here.
 * @Immutable is a pure marker with no runtime behaviour and no effect on the serialized shape of
 * anything under test, which makes it a platform type rather than logic.
 */
@Target(
    AnnotationTarget.CLASS,
    AnnotationTarget.FUNCTION,
    AnnotationTarget.PROPERTY,
    AnnotationTarget.TYPE,
)
@Retention(AnnotationRetention.BINARY)
annotation class Immutable