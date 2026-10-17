package androidx.compose.runtime

/** Stand-in for the Compose stability annotations (compile-only, no behavior). */
@Target(AnnotationTarget.CLASS)
annotation class Immutable

@Target(AnnotationTarget.CLASS)
annotation class Stable
