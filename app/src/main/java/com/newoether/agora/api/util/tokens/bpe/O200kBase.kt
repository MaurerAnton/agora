package com.newoether.agora.api.util.tokens.bpe

import android.content.Context
import com.newoether.agora.api.util.tokens.NativeBpeTextTokenCounter
import com.newoether.agora.api.util.tokens.TextTokenCounter
import com.newoether.agora.core.NativeTokens
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext
import java.util.concurrent.ConcurrentHashMap
import java.util.concurrent.atomic.AtomicBoolean

/**
 * The o200k_base vocabulary, shipped as an asset and held for the life of the process.
 *
 * Counting has to stay synchronous, so the vocabulary is loaded once in the background and only read
 * afterwards. Until it is in memory, [loadedOrNull] returns null and callers fall back to the offline
 * heuristic; that is why loading it early matters but never blocks anything.
 *
 * Two forms are kept from the one asset: the native vocabulary in the C++ core, which counts when
 * its library is up, and the Kotlin [BpeVocabulary] as the fallback. They are exact mirrors — the
 * native side is pinned to the Kotlin one by the parity tests in `core/tests` — so either answers.
 *
 * The asset is the file tiktoken publishes, unchanged, so it can be compared against upstream by
 * checksum.
 */
object O200kBase {

    private const val ASSET_PATH = "tokenizers/o200k_base.tiktoken"
    private const val TAG = "O200kBase"

    @Volatile
    private var vocabulary: BpeVocabulary? = null

    /** Handle of the native vocabulary, or 0 when the native path is not up. */
    @Volatile
    private var nativeVocabulary: Long = 0L

    /** One native counter per factor: each holds a memo that must outlive a single projection. */
    private val nativeCounters = ConcurrentHashMap<Double, TextTokenCounter>()

    private val loading = AtomicBoolean(false)

    /** The Kotlin vocabulary if it is already in memory. Never loads, never blocks. */
    fun loadedOrNull(): BpeVocabulary? = vocabulary

    /**
     * A counter over the native vocabulary for [scale], or null when the native path is not up.
     * Never loads, never blocks; the caller keeps [BpeTextTokenCounter] as the fallback.
     */
    fun nativeCounter(scale: Double = 1.0): TextTokenCounter? {
        if (nativeVocabulary == 0L) return null
        nativeCounters[scale]?.let { return it }
        synchronized(nativeCounters) {
            nativeCounters[scale]?.let { return it }
            if (nativeVocabulary == 0L) return null
            val handle = NativeTokens.createCounter(nativeVocabulary, scale)
            if (handle == 0L) return null
            val counter = NativeBpeTextTokenCounter(handle)
            nativeCounters[scale] = counter
            return counter
        }
    }

    /**
     * Loads the vocabulary unless it is already loaded or another caller is loading it. Reading and
     * parsing three and a half megabytes belongs off the main thread, so it runs on IO.
     */
    suspend fun ensureLoaded(context: Context) {
        if (vocabulary != null || nativeVocabulary != 0L) return
        if (!loading.compareAndSet(false, true)) return
        withContext(Dispatchers.IO) {
            runCatching { readNative(context) }
                .onFailure { error -> DebugLog.w(TAG, "Failed to load the native vocabulary", error) }
                .onSuccess { handle -> nativeVocabulary = handle }
            runCatching { read(context) }
                .onFailure { error -> DebugLog.w(TAG, "Failed to load $ASSET_PATH", error) }
                .onSuccess { loaded -> vocabulary = loaded }
        }
        // A failed load may be retried; a successful one never needs to be.
        if (vocabulary == null && nativeVocabulary == 0L) loading.set(false)
    }

    private fun read(context: Context): BpeVocabulary =
        context.assets.open(ASSET_PATH).bufferedReader().useLines { lines ->
            BpeVocabulary.parseTiktoken(lines)
        }

    private fun readNative(context: Context): Long {
        if (!NativeTokens.available) return 0L
        val bytes = context.assets.open(ASSET_PATH).use { it.readBytes() }
        return NativeTokens.createVocabulary(bytes)
    }
}
