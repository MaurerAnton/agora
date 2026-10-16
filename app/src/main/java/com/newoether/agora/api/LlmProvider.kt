package com.newoether.agora.api

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.TokenUsage
import com.newoether.agora.api.util.prepareMessages
import com.newoether.agora.api.util.tokens.ContextCostModels
import kotlinx.coroutines.flow.Flow
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject

suspend fun ProviderConfig.resolveRequest(messages: List<ChatMessage>): ProviderRequestInput =
    requestResolver?.resolve(messages, this)
        ?: ProviderRequestInput(
            messages = prepareMessages(
                messages,
                maxContextWindow,
                includeAssistantReasoning = includeAssistantReasoning,
                // Trimming must price context the same way the indicator does, so the fallback
                // resolver counts with this model's own cost model too.
                costs = ContextCostModels.forModel(modelId),
            ),
            systemPrompt = systemPrompt,
        )

@Serializable
data class OpenAiResponsesRequest(
    val model: String,
    val input: List<JsonObject>,
    val stream: Boolean = true,
    val tools: List<OpenAiResponseTool>? = null,
    val reasoning: OpenAiReasoning? = null,
    @SerialName("service_tier") val serviceTier: String? = null,
    val temperature: Float? = null,
    @SerialName("max_output_tokens") val maxOutputTokens: Int? = null,
    @SerialName("top_p") val topP: Float? = null,
    @SerialName("prompt_cache_key") val promptCacheKey: String? = null,
)

@Serializable
data class OpenAiResponseInputItem(
    val type: String,
    val id: String? = null,
    val role: String? = null,
    val content: List<OpenAiResponseInputContent>? = null,
    val summary: JsonElement? = null,
    @SerialName("encrypted_content") val encryptedContent: String? = null,
    @SerialName("call_id") val callId: String? = null,
    val name: String? = null,
    val arguments: String? = null,
    val output: JsonElement? = null,
)

@Serializable
data class OpenAiResponseInputContent(
    val type: String,
    val text: String? = null,
    @SerialName("image_url") val imageUrl: String? = null,
    val detail: String? = null,
)

@Serializable
data class OpenAiResponseTool(
    val type: String = "function",
    val name: String? = null,
    val description: String? = null,
    val parameters: ToolParameters? = null,
)

@Serializable
data class OpenAiResponseStreamEvent(
    val type: String,
    val delta: String? = null,
    val arguments: String? = null,
    val name: String? = null,
    @SerialName("item_id") val itemId: String? = null,
    @SerialName("output_index") val outputIndex: Int? = null,
    @SerialName("content_index") val contentIndex: Int? = null,
    @SerialName("summary_index") val summaryIndex: Int? = null,
    @SerialName("sequence_number") val sequenceNumber: Int? = null,
    val item: JsonObject? = null,
    val annotation: OpenAiResponseAnnotation? = null,
    val response: OpenAiResponseEnvelope? = null,
    val error: OpenAiError? = null,
)

@Serializable
data class OpenAiResponseAnnotation(
    val type: String,
    val title: String? = null,
    val url: String? = null,
    @SerialName("start_index") val startIndex: Int? = null,
    @SerialName("end_index") val endIndex: Int? = null,
    val index: Int? = null,
    @SerialName("file_id") val fileId: String? = null,
    val filename: String? = null,
    @SerialName("container_id") val containerId: String? = null,
)

@Serializable
data class OpenAiResponseOutputItem(
    val id: String? = null,
    val type: String? = null,
    val summary: JsonElement? = null,
    @SerialName("encrypted_content") val encryptedContent: String? = null,
    @SerialName("call_id") val callId: String? = null,
    val name: String? = null,
    val arguments: String? = null,
)

@Serializable
data class OpenAiResponseEnvelope(
    val status: String? = null,
    val error: OpenAiError? = null,
    @SerialName("incomplete_details") val incompleteDetails: OpenAiResponseIncompleteDetails? = null,
    val usage: OpenAiResponseUsage? = null,
)

@Serializable
data class OpenAiResponseIncompleteDetails(val reason: String? = null)

@Serializable
data class OpenAiResponseUsage(
    @SerialName("input_tokens") val inputTokens: Int? = null,
    @SerialName("output_tokens") val outputTokens: Int? = null,
    @SerialName("total_tokens") val totalTokens: Int? = null,
    @SerialName("cache_write_tokens") val cacheWriteTokens: Int? = null,
    @SerialName("input_tokens_details") val inputTokensDetails: OpenAiResponseInputTokenDetails? = null,
    @SerialName("output_tokens_details") val outputTokensDetails: OpenAiResponseOutputTokenDetails? = null,
)

@Serializable
data class OpenAiResponseInputTokenDetails(
    @SerialName("cached_tokens") val cachedTokens: Int? = null,
)

@Serializable
data class OpenAiResponseOutputTokenDetails(
    @SerialName("reasoning_tokens") val reasoningTokens: Int? = null,
)

internal fun OpenAiResponseUsage.toTokenUsage(): TokenUsage {
    val input = inputTokens?.coerceAtLeast(0)
    val cached = inputTokensDetails?.cachedTokens?.coerceAtLeast(0)
    val output = outputTokens?.coerceAtLeast(0)
    return TokenUsage(
        totalTokenCount = (totalTokens ?: listOfNotNull(input, output).sum()).coerceAtLeast(0),
        inputTokenCount = input,
        cachedInputTokenCount = cached,
        cacheWriteInputTokenCount = cacheWriteTokens?.coerceAtLeast(0),
        uncachedInputTokenCount = if (input != null && cached != null) {
            (input - cached).coerceAtLeast(0)
        } else null,
        outputTokenCount = output,
        reasoningTokenCount = outputTokensDetails?.reasoningTokens?.coerceAtLeast(0),
    )
}

@Serializable
data class OpenAiTool(
    val type: String,
    val function: OpenAiFunction? = null
)

@Serializable
data class OpenAiFunction(
    val name: String,
    val description: String? = null,
    val parameters: JsonObject? = null
)

interface LlmProvider {
    val name: String
    val defaultBaseUrl: String
    val nativeTextParsingAuthoritative: Boolean
        get() = false
    val baseUrlPlaceholder: String
        get() = defaultBaseUrl

    fun generateResponse(
        messages: List<ChatMessage>,
        config: ProviderConfig
    ): Flow<StreamEvent>
    
    suspend fun fetchModels(apiKey: String, baseUrl: String? = null): List<String>
}
