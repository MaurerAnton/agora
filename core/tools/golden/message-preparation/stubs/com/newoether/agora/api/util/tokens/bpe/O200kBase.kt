package com.newoether.agora.api.util.tokens.bpe

import com.newoether.agora.api.util.tokens.TextTokenCounter
import java.io.File

/**
 * Host-stand replacement for the Android loader.
 *
 * The vocabulary comes from the same shipped tiktoken asset file (the driver injects its path),
 * and the native counter is always absent — exactly the app's state with the native library down,
 * where BpeTextTokenCounter answers through the Kotlin implementation. Counting semantics stay in
 * the real BpeVocabulary/BpeTokenCount sources compiled alongside this stub.
 */
object O200kBase {

    @Volatile
    private var vocabulary: BpeVocabulary? = null

    /** The Kotlin vocabulary if it is already in memory. Never loads, never blocks. */
    fun loadedOrNull(): BpeVocabulary? = vocabulary

    /** No JNI on the host: the Kotlin counter is the only path, like the app before native init. */
    fun nativeCounter(scale: Double = 1.0): TextTokenCounter? = null

    /** Stand-in for ensureLoaded(context): reads the asset from a host path. */
    fun loadFromFile(path: String) {
        vocabulary = File(path).bufferedReader().useLines { lines ->
            BpeVocabulary.parseTiktoken(lines)
        }
    }
}
