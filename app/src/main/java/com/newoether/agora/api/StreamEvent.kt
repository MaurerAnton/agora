package com.newoether.agora.api

import com.newoether.agora.model.CitationRecord
import com.newoether.agora.model.TokenUsage
import kotlinx.serialization.json.JsonObject

sealed class StreamEvent {
    data class TextChunk(val text: String) : StreamEvent()
    data class CitationUpdate(val citation: CitationRecord) : StreamEvent()
    data class ThoughtChunk(val thought: String, val title: String? = null, val signature: String? = null) : StreamEvent()
    data class UsageUpdate(val usage: TokenUsage) : StreamEvent() {
        constructor(tokenCount: Int, thoughtsTokenCount: Int = 0) : this(
            TokenUsage(
                totalTokenCount = tokenCount.coerceAtLeast(0),
                reasoningTokenCount = thoughtsTokenCount
                    .takeIf { it > 0 },
            )
        )

        val tokenCount: Int
            get() = usage.totalTokenCount

        val thoughtsTokenCount: Int
            get() = usage.reasoningTokenCount ?: 0
    }
    data class Error(val error: GenerationError) : StreamEvent() {
        val message: String get() = error.userMessage()
    }
    /**
     * Full accumulated snapshot of a tool call while the model is still writing it.
     *
     * [streamKey] is stable even when a compatible provider sends the protocol [id] in a later
     * delta. UI code keys the live segment by [streamKey], then replaces its protocol id when the
     * matching [ToolCallRequest] completes.
     */
    data class ToolCallUpdate(
        val streamKey: String,
        val id: String?,
        val name: String,
        val arguments: String,
        val signature: String? = null,
    ) : StreamEvent()

    /**
     * Display-only progress for a tool executed inside the provider transport.
     *
     * Unlike [ToolCallRequest], this event never authorizes local execution or a continuation
     * round. A non-null [result] closes the provider-hosted call into a terminal UI segment.
     */
    data class HostedToolCallUpdate(
        val streamKey: String,
        val name: String,
        val arguments: String,
        val result: String? = null,
        val isError: Boolean = false,
    ) : StreamEvent()

    data class ToolCallRequest(
        val id: String,
        val name: String,
        val arguments: String,
        val signature: String? = null,
        val streamKey: String = id,
        /** Raw Responses output items that must precede this call during local-tool continuation. */
        val responseOutputItems: List<JsonObject> = emptyList(),
    ) : StreamEvent()
    data class ToolCallsRequest(val calls: List<ToolCallRequest>) : StreamEvent()
    /** Emitted before one provider retry. [attempt] is the 1-based retry number, while
     *  [maxAttempts] is the retry budget and excludes the initial request. */
    data class Retrying(val attempt: Int, val maxAttempts: Int) : StreamEvent()
}
