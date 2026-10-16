package com.newoether.agora.api.openai

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.util.StreamTermination
import com.newoether.agora.model.TokenUsage
import java.nio.ByteBuffer

/**
 * Decoder for the binary wire format documented in
 * `core/include/agora/core/openai_stream_codec.hpp`: everything `NativeOpenAiChatStreamConsumer`
 * receives from the native consumer arrives as one of these payloads.
 *
 * The format is pinned from both ends by the committed fixtures under
 * `app/src/test/resources/openai_stream_codec` (generator:
 * `core/tools/golden/openai/codec/generate_codec_fixtures.py`): the native encoder test compares
 * its bytes to the fixtures, [NativeStreamCodecTest] decodes the same bytes back into events.
 * Every read is bounds-checked, so a format drift fails loudly instead of corrupting a stream.
 */
internal object NativeStreamCodec {

    class LineOutcome(
        val keepReading: Boolean,
        val logs: List<String>,
        val events: List<StreamEvent>,
    )

    fun decodeLineOutcome(bytes: ByteArray): LineOutcome {
        val buffer = ByteBuffer.wrap(bytes)
        val keepReading = buffer.readFlag()
        val logs = List(buffer.readU16()) { buffer.readString() }
        val events = List(buffer.readU16()) { buffer.readEvent() }
        buffer.requireFullyConsumed()
        return LineOutcome(keepReading, logs, events)
    }

    fun decodeTermination(bytes: ByteArray): StreamTermination {
        val buffer = ByteBuffer.wrap(bytes)
        val termination = StreamTermination(
            sawTerminalMarker = buffer.readFlag(),
            stopReason = buffer.readNullableString(),
            producedContent = buffer.readFlag(),
            toolCallInFlight = buffer.readFlag(),
            alreadyReportedError = buffer.readFlag(),
            timedOut = buffer.readFlag(),
            streamError = buffer.readError(),
        )
        buffer.requireFullyConsumed()
        return termination
    }

    // ── helpers ──────────────────────────────────────────────────────────────

    private fun ByteBuffer.requireFullyConsumed() {
        require(!hasRemaining()) { "native stream codec: ${remaining()} trailing bytes" }
    }

    private fun ByteBuffer.readFlag(): Boolean {
        require(hasRemaining()) { "native stream codec: truncated flag" }
        return get().toInt() != 0
    }

    private fun ByteBuffer.readU16(): Int {
        require(remaining() >= 2) { "native stream codec: truncated u16" }
        val high = get().toInt() and 0xFF
        val low = get().toInt() and 0xFF
        return (high shl 8) or low
    }

    /** A string length; -1 marks a null string and is only read through [readNullableString]. */
    private fun ByteBuffer.readU32(): Int {
        require(remaining() >= 4) { "native stream codec: truncated u32" }
        return getInt()
    }

    private fun ByteBuffer.readI32(): Int {
        require(remaining() >= 4) { "native stream codec: truncated i32" }
        return getInt()
    }

    private fun ByteBuffer.readOptionalI32(): Int? = if (readFlag()) readI32() else null

    private fun ByteBuffer.readString(): String {
        val length = readU32()
        require(length >= 0 && length <= remaining()) {
            "native stream codec: bad string length $length"
        }
        val bytes = ByteArray(length)
        get(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    private fun ByteBuffer.readNullableString(): String? {
        val marker = readU32()
        if (marker == -1) return null
        require(marker >= 0 && marker <= remaining()) {
            "native stream codec: bad string length $marker"
        }
        val bytes = ByteArray(marker)
        get(bytes)
        return String(bytes, Charsets.UTF_8)
    }

    private fun ByteBuffer.readUsage(): TokenUsage = TokenUsage(
        totalTokenCount = readI32(),
        inputTokenCount = readOptionalI32(),
        cachedInputTokenCount = readOptionalI32(),
        cacheWriteInputTokenCount = readOptionalI32(),
        uncachedInputTokenCount = readOptionalI32(),
        outputTokenCount = readOptionalI32(),
        reasoningTokenCount = readOptionalI32(),
    )

    private fun ByteBuffer.readToolCallRequest(): StreamEvent.ToolCallRequest {
        val id = readString()
        val name = readString()
        val arguments = readString()
        val signature = readNullableString()
        val streamKey = readString()
        return StreamEvent.ToolCallRequest(
            id = id,
            name = name,
            arguments = arguments,
            signature = signature,
            streamKey = streamKey,
        )
    }

    private fun ByteBuffer.readEvent(): StreamEvent {
        require(hasRemaining()) { "native stream codec: truncated event" }
        return when (val kind = get().toInt()) {
            1 -> StreamEvent.TextChunk(text = readString())
            2 -> StreamEvent.ThoughtChunk(
                thought = readString(),
                title = readNullableString(),
                signature = readNullableString(),
            )
            3 -> StreamEvent.ToolCallUpdate(
                streamKey = readString(),
                id = readNullableString(),
                name = readString(),
                arguments = readString(),
            )
            4 -> readToolCallRequest()
            5 -> StreamEvent.ToolCallsRequest(
                calls = List(readU16()) { readToolCallRequest() },
            )
            6 -> StreamEvent.UsageUpdate(usage = readUsage())
            7 -> StreamEvent.Error(
                error = readError() ?: error("native stream codec: error event without an error"),
            )
            else -> error("native stream codec: unknown event kind $kind")
        }
    }

    private fun ByteBuffer.readError(): GenerationError? {
        require(hasRemaining()) { "native stream codec: truncated error" }
        return when (val kind = get().toInt()) {
            0 -> null
            1 -> GenerationError.Network(
                statusCode = readI32(),
                message = readString(),
            )
            2 -> GenerationError.Api(
                code = readNullableString(),
                type = readNullableString(),
                message = readString(),
            )
            3 -> GenerationError.SseParse(
                rawLine = readString(),
                cause = readString(),
            )
            4 -> GenerationError.MalformedToolCall(cause = readString())
            5 -> GenerationError.IncompleteStream(
                provider = readString(),
                stopReason = readNullableString(),
                toolCallInFlight = readFlag(),
                producedContent = readFlag(),
            )
            6 -> GenerationError.OutputTruncated(
                provider = readString(),
                stopReason = readNullableString(),
            )
            7 -> GenerationError.Timeout
            else -> error("native stream codec: unknown error kind $kind")
        }
    }
}
