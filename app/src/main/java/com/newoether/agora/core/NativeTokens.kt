package com.newoether.agora.core

/**
 * The JNI door into the C++ core (top-level `core/`, built as `libagora_core.so`).
 *
 * Everything here is best-effort: when the native library cannot load — desktop unit tests,
 * unusual builds — [available] is false and callers keep the Kotlin implementation. That is what
 * makes the cutover verifiable rather than mysterious.
 *
 * Handles are process-lifetime: the vocabulary is created once at startup and counters are kept
 * per factor, because their memos must outlive a single context projection.
 */
object NativeTokens {

    val available: Boolean = runCatching { System.loadLibrary("agora_core") }.isSuccess

    external fun createVocabulary(bytes: ByteArray): Long

    external fun destroyVocabulary(vocabulary: Long)

    external fun createCounter(vocabulary: Long, scale: Double): Long

    external fun destroyCounter(counter: Long)

    external fun count(counter: Long, text: String): Long
}
