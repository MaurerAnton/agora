package com.newoether.agora.api.openai

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.util.StreamTermination
import com.newoether.agora.core.NativeOpenAiStream
import com.newoether.agora.util.DebugLog

/**
 * The native twin of [OpenAiChatStreamConsumer]: same interface, same semantics, with the
 * per-line meaning decided by `agora::core::openai::ChatStreamConsumer` and pinned to the Kotlin
 * reference by the 91 golden traces under `core/tools/golden/openai`.
 *
 * Only the base delta interpretation is implemented natively, so a provider with its own
 * [OpenAiChatStreamConsumer] parseDelta (OpenRouter) keeps the Kotlin reference. Instances are
 * one per stream attempt and must be [close]d; the handle owns the pending tool calls.
 */
internal class NativeOpenAiChatStreamConsumer private constructor(
    private val handle: Long,
    private val logMalformed: (String) -> Unit,
) : OpenAiChatStreamConsumption, AutoCloseable {

    private var closed = false

    override val hasSemanticTerminal: Boolean
        get() = NativeOpenAiStream.hasSemanticTerminal(handle)

    override suspend fun consumeLine(line: String, emit: suspend (StreamEvent) -> Unit): Boolean {
        val outcome = NativeStreamCodec.decodeLineOutcome(
            NativeOpenAiStream.consumeLine(handle, line) ?: nativeFailure(),
        )
        // The reference logs each malformed payload through the same lambda; the native consumer
        // builds the identical debug line and returns it with the call's outcome.
        outcome.logs.forEach(logMalformed)
        for (event in outcome.events) emit(event)
        return outcome.keepReading
    }

    override fun onReadTimeout(terminalGraceActive: Boolean): Boolean =
        NativeOpenAiStream.onReadTimeout(handle, terminalGraceActive)

    override fun onReadError(error: GenerationError) {
        when (error) {
            is GenerationError.Network -> {
                NativeOpenAiStream.onReadError(handle, error.statusCode, error.message)
            }
            // The provider maps every response-body read failure to Network before handing it
            // over; anything else means the contract changed. Degrade loudly instead of lying:
            // the stream still terminates correctly, only the error's shape is approximate.
            else -> {
                DebugLog.e(
                    "AgoraAPI",
                    "native stream consumer received unexpected read error " +
                        "${error::class.simpleName}",
                )
                NativeOpenAiStream.onReadError(handle, 0, error.userMessage())
            }
        }
    }

    override fun finish(): StreamTermination =
        NativeStreamCodec.decodeTermination(
            NativeOpenAiStream.finish(handle) ?: nativeFailure(),
        )

    override fun close() {
        if (closed) return
        closed = true
        NativeOpenAiStream.destroy(handle)
    }

    private fun nativeFailure(): Nothing =
        error("native stream consumer failed; the JNI bridge returned no payload")

    companion object {
        /** Returns null when the native consumer cannot be created, leaving the Kotlin fallback. */
        fun createOrNull(
            providerName: String,
            logMalformed: (String) -> Unit,
        ): NativeOpenAiChatStreamConsumer? {
            if (!NativeOpenAiStream.available) return null
            val handle = NativeOpenAiStream.create(providerName)
            if (handle == 0L) {
                DebugLog.e("AgoraAPI", "native stream consumer could not be created")
                return null
            }
            return NativeOpenAiChatStreamConsumer(handle, logMalformed)
        }
    }
}
