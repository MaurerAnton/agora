package com.newoether.agora.core

/**
 * The JNI door into the native OpenAI chat stream consumer (core/src/openai_stream.cpp).
 *
 * Everything here is best-effort, exactly like [NativeTokens]: when the native library cannot
 * load — desktop unit tests, unusual builds — [available] is false and providers keep the
 * Kotlin reference consumer. Handles are per stream attempt: [create] runs once before the read
 * loop and [destroy] once after it, on every path including cancellation.
 *
 * Payloads crossing the bridge are the binary codec documented in
 * core/include/agora/core/openai_stream_codec.hpp, decoded by
 * [com.newoether.agora.api.openai.NativeStreamCodec].
 */
internal object NativeOpenAiStream {

    val available: Boolean = runCatching { System.loadLibrary("agora_core") }.isSuccess

    /** Returns a handle, or 0 when the consumer could not be created. */
    external fun create(providerName: String): Long

    external fun destroy(handle: Long)

    external fun consumeLine(handle: Long, line: String): ByteArray

    external fun onReadTimeout(handle: Long, terminalGraceActive: Boolean): Boolean

    /** The only read failure the provider maps for the consumer is a [com.newoether.agora.api.GenerationError.Network]. */
    external fun onReadError(handle: Long, statusCode: Int, message: String)

    external fun hasSemanticTerminal(handle: Long): Boolean

    external fun finish(handle: Long): ByteArray
}
