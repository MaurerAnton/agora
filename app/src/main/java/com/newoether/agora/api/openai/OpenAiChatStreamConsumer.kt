package com.newoether.agora.api.openai

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.OpenAiDelta
import com.newoether.agora.api.OpenAiStreamResponse
import com.newoether.agora.api.PendingToolCall
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.toTokenUsage
import com.newoether.agora.api.util.ProviderRetryPolicy
import com.newoether.agora.api.util.StreamTermination
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.api.util.malformedToolCallRequest
import com.newoether.agora.api.util.safeWireToolCallId
import com.newoether.agora.api.util.safeWireToolName
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

/**
 * One way to consume one HTTP 200 chat-completions SSE stream and report how it went.
 *
 * The provider owns the socket, the clock and the cancellation; this interface owns everything
 * that decides what the bytes MEAN: delta parsing, tool-call accumulation, the `[DONE]` /
 * `finish_reason` terminal signals, usage mapping and the [StreamTermination] verdict.
 *
 * [OpenAiChatStreamConsumer] is the Kotlin reference; the native port must behave identically
 * event for event. Both are pinned together by the golden traces under `core/tools/golden/openai`.
 */
internal interface OpenAiChatStreamConsumption {
    /** True once a `finish_reason` arrived, which starts the provider's terminal grace window. */
    val hasSemanticTerminal: Boolean

    /** Feed one raw line; returns false when the read loop must stop. */
    suspend fun consumeLine(line: String, emit: suspend (StreamEvent) -> Unit): Boolean

    /**
     * A read timed out. Returns false when the loop must stop: either the terminal grace tail
     * ended (success) or the connection has been silent for the full timeout budget.
     */
    fun onReadTimeout(terminalGraceActive: Boolean): Boolean

    /** A response-body read failure already mapped to a retryable [GenerationError]. */
    fun onReadError(error: GenerationError)

    /** How the stream ended. Only valid after the read loop stopped. */
    fun finish(): StreamTermination
}

/**
 * Base delta interpretation: `reasoning_content` is the vLLM/DeepSeek-compatible field;
 * `reasoning` is the bare-string form many relays emit instead. Take whichever the endpoint
 * actually populated. Kept here (not only as an overridable provider method) so the golden
 * harness pins the exact real implementation.
 */
internal suspend fun defaultOpenAiDeltaContent(
    delta: OpenAiDelta,
    emit: suspend (StreamEvent) -> Unit,
) {
    val reasoning = delta.reasoningContent?.takeIf(String::isNotEmpty)
        ?: delta.reasoning?.takeIf(String::isNotEmpty)
    reasoning?.let {
        emit(StreamEvent.ThoughtChunk(it))
    }
    delta.content?.takeIf(String::isNotEmpty)?.let { content ->
        emit(StreamEvent.TextChunk(content))
    }
}

/**
 * Consumes one OpenAI-compatible chat stream. Moved out of [BaseOpenAiProvider] unchanged: the
 * provider's read loop feeds lines in, and this class decides what they mean.
 */
internal class OpenAiChatStreamConsumer(
    private val json: Json,
    private val providerName: String,
    private val parseDelta: suspend (OpenAiDelta, suspend (StreamEvent) -> Unit) -> Unit =
        ::defaultOpenAiDeltaContent,
    /** Receives the exact `DebugLog.e("AgoraAPI", ...)` message the provider used to log. */
    private val logMalformed: (String) -> Unit = {},
) : OpenAiChatStreamConsumption {

    private val pendingToolCalls = mutableMapOf<Int, PendingToolCall>()
    private var producedContent = false
    private var reportedError = false
    private var streamError: GenerationError? = null
    private var finishReason: String? = null
    private var sawDone = false
    private var timedOut = false

    // Read timeouts are tolerated for long thinking pauses, but a silently-dead connection
    // (NAT drop with no RST) must not hang forever: give up after 3 consecutive timeouts.
    private var consecutiveReadTimeouts = 0

    override val hasSemanticTerminal: Boolean
        get() = finishReason != null

    override fun onReadTimeout(terminalGraceActive: Boolean): Boolean {
        // A semantic terminal event already completed the model response. This timeout only
        // closes its optional usage/[DONE] grace tail and is therefore success.
        if (terminalGraceActive) return false
        consecutiveReadTimeouts++
        if (consecutiveReadTimeouts >= 3) {
            timedOut = true
            return false
        }
        return true
    }

    override fun onReadError(error: GenerationError) {
        streamError = error
    }

    override suspend fun consumeLine(line: String, emit: suspend (StreamEvent) -> Unit): Boolean {
        consecutiveReadTimeouts = 0
        if (!line.startsWith("data: ")) return true
        val jsonStr = line.substring(6).trim()

        val emitTracked: suspend (StreamEvent) -> Unit = { event ->
            if (event.carriesModelOutput()) producedContent = true
            if (event is StreamEvent.Error) reportedError = true
            emit(event)
        }

        if (jsonStr == "[DONE]") {
            sawDone = true
            emitPendingStructuredToolCalls(emitTracked)
            return false
        }

        try {
            val response = json.decodeFromString<OpenAiStreamResponse>(jsonStr)

            // A relay signals a mid-stream failure as a bare {"error":{...}} chunk on a 200
            // response. Previously ignoreUnknownKeys discarded it and the generation just
            // stopped with no diagnostic at all.
            response.error?.let { error ->
                streamError = GenerationError.Api(
                    code = error.code,
                    type = error.type,
                    message = error.message.ifBlank {
                        "Provider reported an error in the response stream"
                    },
                )
            }
            if (
                streamError == null &&
                ProviderRetryPolicy.isFailedToGenerateOutcome(response.outcome)
            ) {
                streamError = GenerationError.Api(
                    code = null,
                    type = "failed_to_generate",
                    message = response.outcome.orEmpty(),
                )
            }

            val choice = response.choices?.firstOrNull()

            choice?.delta?.let { delta ->
                parseDelta(delta, emitTracked)

                delta.toolCalls?.forEach { tc ->
                    val existingEntry = tc.id?.let { id ->
                        pendingToolCalls.entries.firstOrNull { it.value.id == id }
                    }
                    val index = tc.index
                        ?: existingEntry?.key
                        ?: pendingToolCalls.keys.singleOrNull()
                        ?: pendingToolCalls.size
                    val pending = pendingToolCalls.getOrPut(index) { PendingToolCall() }
                    tc.id?.takeIf(String::isNotBlank)?.let { pending.id = it }
                    tc.function?.name?.takeIf(String::isNotBlank)?.let { pending.name = it }
                    tc.function?.arguments?.let {
                        // Snapshot-tolerant: a relay that resends the whole argument string in
                        // every delta must not produce `{"a":1}{"a":1}`, and an empty
                        // placeholder delta must not erase what was already accumulated.
                        pending.args.append(
                            if (it is JsonPrimitive) it.content else it.toString()
                        )
                    }
                    emitTracked(
                        StreamEvent.ToolCallUpdate(
                            streamKey = pending.streamKey,
                            id = pending.id.ifBlank { null },
                            name = pending.name,
                            arguments = pending.args.toString(),
                        )
                    )
                }
            }

            choice?.finishReason?.takeIf(String::isNotBlank)?.let {
                finishReason = it.lowercase()
            }

            if (!choice?.finishReason.isNullOrBlank()) {
                // Several OpenAI-compatible gateways return "stop" (or close directly)
                // after streaming a perfectly valid structured tool call. The accumulated
                // call is authoritative; never discard it based on the terminal spelling.
                emitPendingStructuredToolCalls(emitTracked)
            }

            response.usage?.let { usage ->
                emitTracked(StreamEvent.UsageUpdate(usage.toTokenUsage()))
            }

            if (streamError != null) return false

            return true
        } catch (e: Exception) {
            logMalformed("[$providerName] malformed stream payload exception=${e.javaClass.simpleName}")
            streamError = GenerationError.SseParse(
                rawLine = jsonStr.take(512),
                cause = e.localizedMessage ?: "Malformed SSE payload",
            )
            return false
        }
    }

    override fun finish(): StreamTermination = StreamTermination(
        // Either signal proves the message ended semantically. Many gateways omit one of them,
        // so requiring both would report false truncations.
        sawTerminalMarker = sawDone || finishReason != null,
        stopReason = finishReason,
        producedContent = producedContent,
        toolCallInFlight = pendingToolCalls.isNotEmpty(),
        streamError = streamError,
        alreadyReportedError = reportedError,
        timedOut = timedOut,
    )

    private suspend fun emitPendingStructuredToolCalls(emitTracked: suspend (StreamEvent) -> Unit) {
        if (pendingToolCalls.isEmpty()) return
        val pending = pendingToolCalls.values.toList()
        pendingToolCalls.clear()
        val seenIds = mutableSetOf<String>()
        val calls = pending.map { candidate ->
            val callId = candidate.id.ifBlank { candidate.streamKey }
            val arguments = candidate.args.toString().ifBlank { "{}" }
            val argumentsAreObject = runCatching {
                json.parseToJsonElement(arguments) is JsonObject
            }.getOrDefault(false)
            // A damaged call becomes a stand-in the executor answers with this cause, so the
            // model can correct itself instead of the whole run failing.
            val cause = when {
                !callId.matches(safeWireToolCallId) -> "Provider returned an invalid tool call id"
                !seenIds.add(callId) -> "Provider returned duplicate tool call ids"
                !candidate.name.matches(safeWireToolName) ->
                    "Provider ended before the tool name was complete"
                !argumentsAreObject ->
                    "Provider ended before the tool arguments formed a complete JSON object"
                else -> null
            }
            if (cause == null) {
                StreamEvent.ToolCallRequest(
                    id = callId,
                    name = candidate.name,
                    arguments = arguments,
                    streamKey = candidate.streamKey,
                )
            } else {
                malformedToolCallRequest(cause, candidate.name, arguments, candidate.streamKey)
            }
        }
        if (calls.size == 1) emitTracked(calls.first())
        else emitTracked(StreamEvent.ToolCallsRequest(calls))
    }
}
