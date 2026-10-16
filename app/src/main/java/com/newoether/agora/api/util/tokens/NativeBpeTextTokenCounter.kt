package com.newoether.agora.api.util.tokens

import com.newoether.agora.core.NativeTokens

/**
 * Text counting through the C++ core's BPE port (`core/`, `libagora_core.so`).
 *
 * The vocabulary lookup and the merge loop — what the Kotlin [BpeTextTokenCounter] does on the
 * JVM — run natively here; this class only routes the string across JNI and keeps the heuristic
 * for the degenerate handle. Parity with the Kotlin implementation is frozen in `core/tests`:
 * tiktoken's own reference set plus a fuzz corpus with golden counts. The family factor is baked
 * into the native counter at creation, so it cannot be forgotten per call.
 */
class NativeBpeTextTokenCounter internal constructor(
    private val counter: Long,
    private val fallback: TextTokenCounter = HeuristicTextTokenCounter,
) : TextTokenCounter {

    override fun count(text: String): Long {
        if (text.isEmpty()) return 0L
        if (counter == 0L) return fallback.count(text)
        return NativeTokens.count(counter, text)
    }
}
