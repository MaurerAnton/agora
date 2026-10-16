package com.newoether.agora.api

import com.newoether.agora.model.TokenUsage
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement

@Serializable
data class OpenAiStreamResponse(
    val id: String? = null,
    val choices: List<OpenAiChoice>? = null,
    /** Non-standard relay outcome. Some gateways return `failed to generate` here on HTTP 200. */
    val outcome: String? = null,
    // Relays deliver a mid-stream failure as a bare {"error":{...}} chunk on an HTTP 200 response.
    // Without this field `ignoreUnknownKeys` discarded it, choices stayed null, and the generation
    // simply stopped with no diagnostic at all.
    val error: OpenAiError? = null,
    val usage: OpenAiUsage? = null
)

@Serializable
data class OpenAiChoice(
    val index: Int,
    val delta: OpenAiDelta? = null,
    @SerialName("finish_reason") val finishReason: String? = null
)

@Serializable
data class OpenAiDelta(
    val role: String? = null,
    val content: String? = null,
    @SerialName("reasoning_content") val reasoningContent: String? = null,
    // Bare `reasoning` string: OpenRouter and many Claude/DeepSeek-in-OpenAI relays emit this
    // instead of (or duplicated alongside) reasoning_content. Read as a fallback so a
    // non-standard endpoint's thinking is never silently dropped.
    val reasoning: String? = null,
    @SerialName("reasoning_details") val reasoningDetails: List<OpenAiReasoningDetail>? = null,
    @SerialName("tool_calls") val toolCalls: List<OpenAiToolCall>? = null
)

@Serializable
data class OpenAiReasoningDetail(
    val type: String? = null,
    val text: String? = null
)

@Serializable
data class OpenAiToolCall(
    val index: Int? = null,
    val id: String? = null,
    val type: String? = null,
    val function: OpenAiFunctionCall? = null
)

@Serializable
data class OpenAiFunctionCall(
    val name: String? = null,
    val arguments: JsonElement? = null
)

@Serializable
data class OpenAiUsage(
    @SerialName("prompt_tokens") val promptTokens: Int? = null,
    @SerialName("completion_tokens") val completionTokens: Int? = null,
    @SerialName("total_tokens") val totalTokens: Int? = null,
    @SerialName("prompt_tokens_details") val promptTokensDetails: OpenAiPromptTokensDetails? = null,
    @SerialName("prompt_cache_hit_tokens") val promptCacheHitTokens: Int? = null,
    @SerialName("prompt_cache_miss_tokens") val promptCacheMissTokens: Int? = null,
    @SerialName("cache_write_tokens") val cacheWriteTokens: Int? = null,
    @SerialName("completion_tokens_details") val completionTokensDetails: OpenAiCompletionTokensDetails? = null
)

@Serializable
data class OpenAiPromptTokensDetails(
    @SerialName("cached_tokens") val cachedTokens: Int? = null
)

@Serializable
data class OpenAiCompletionTokensDetails(
    @SerialName("reasoning_tokens") val reasoningTokens: Int? = null
)

internal fun OpenAiUsage.toTokenUsage(): TokenUsage {
    val input = promptTokens?.coerceAtLeast(0)
    val cached = (
        promptCacheHitTokens
            ?: promptTokensDetails?.cachedTokens
        )?.coerceAtLeast(0)
    val uncached = (
        promptCacheMissTokens
            ?: if (input != null && cached != null) {
                (input - cached).coerceAtLeast(0)
            } else {
                null
            }
        )?.coerceAtLeast(0)
    val output = completionTokens?.coerceAtLeast(0)
    val reasoning = completionTokensDetails?.reasoningTokens?.coerceAtLeast(0)
    val derivedTotal = listOfNotNull(input, output).sum()
    return TokenUsage(
        totalTokenCount = (totalTokens ?: derivedTotal).coerceAtLeast(0),
        inputTokenCount = input ?: if (cached != null && uncached != null) {
            TokenUsage.addCounts(cached, uncached)
        } else {
            null
        },
        cachedInputTokenCount = cached,
        cacheWriteInputTokenCount = cacheWriteTokens?.coerceAtLeast(0),
        uncachedInputTokenCount = uncached,
        outputTokenCount = output,
        reasoningTokenCount = reasoning,
    )
}

@Serializable
data class OpenAiModelListResponse(
    val data: List<OpenAiModelInfo>,
    @SerialName("has_more") val hasMore: Boolean = false,
    @SerialName("last_id") val lastId: String? = null
)

@Serializable
data class OpenAiModelInfo(val id: String)

@Serializable
data class OpenAiErrorResponse(val error: OpenAiError)

@Serializable
data class OpenAiError(val message: String, val type: String? = null, val code: String? = null)

class PendingToolCall(
    val streamKey: String = "call_stream_${java.util.UUID.randomUUID()}",
    var id: String = "",
    var name: String = "",
    /** Snapshot-tolerant accumulator: a relay that resends the whole value in every delta, or
     *  interleaves empty placeholder deltas, must not corrupt or erase the arguments. */
    val args: com.newoether.agora.api.util.ToolArgumentAccumulator =
        com.newoether.agora.api.util.ToolArgumentAccumulator()
)