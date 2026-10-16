package com.newoether.agora.api

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.ContextBudget
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.Transient
import kotlinx.serialization.json.JsonObject

/**
 * Request models shared by every provider transport. This file is deliberately free of runtime
 * plumbing (no coroutines, no HTTP): the golden harness under `core/tools/golden/openai-request`
 * compiles it straight from the app sources to pin the chat-completions wire format.
 */
data class ProviderRequestInput(
    val messages: List<ChatMessage>,
    val systemPrompt: String?,
)

fun interface ProviderRequestResolver {
    suspend fun resolve(
        messages: List<ChatMessage>,
        config: ProviderConfig,
    ): ProviderRequestInput
}

data class ProviderConfig(
    val apiKey: String,
    val modelId: String,
    val systemPrompt: String? = null,
    /** Estimated provider-visible conversation token budget. */
    val maxContextWindow: Int = ContextBudget.DEFAULT_TOKENS,
    val codeExecutionEnabled: Boolean = false,
    val googleSearchEnabled: Boolean = false,
    val thinkingEnabled: Boolean = true,
    val thinkingLevel: String = "medium",
    val thinkingBudgetEnabled: Boolean = false,
    val thinkingBudgetTokens: Int = 4096,
    val openAiServiceTier: String? = null,
    val responsesApiEnabled: Boolean = false,
    val anthropicCacheEnabled: Boolean = true,
    val anthropicCacheTtl: String = "1h",
    val openAiWebSearchEnabled: Boolean = false,
    val baseUrl: String? = null,
    val tools: List<ToolDefinition>? = null,
    val userPrepend: String? = null,
    val userPostpend: String? = null,
    val includeImages: Boolean = true,
    /** Counts and retains ordinary assistant thought segments that this request serializes. */
    val includeAssistantReasoning: Boolean = false,
    val temperature: Float? = null,
    val maxTokens: Int? = null,
    val topP: Float? = null,
    val frequencyPenalty: Float? = null,
    val presencePenalty: Float? = null,
    /** Stable cache partition key. Set only for the official OpenAI provider. */
    val promptCacheKey: String? = null,
    /** Stable per-conversation session id. Sent as the `x-opencode-session` header. */
    val sessionId: String? = null,
    /** Resolves ordinary-generation prompt variables and rollout immediately before dispatch. */
    val requestResolver: ProviderRequestResolver? = null,
)

@Serializable
data class ToolDefinition(
    val type: String = "function",
    val function: ToolFunction
)

@Serializable
data class ToolFunction(
    val name: String,
    val description: String,
    val parameters: ToolParameters
)

@Serializable(with = ToolParametersSerializer::class)
data class ToolParameters(
    val type: String = "object",
    val properties: Map<String, ToolProperty>,
    val required: List<String> = emptyList(),
    /**
     * The parameter schema exactly as an external tool source (an MCP server) declared it. When set,
     * every provider sends this schema instead of one rebuilt from [properties], so nested fields,
     * maps, enums and other JSON Schema keywords survive; [properties] and [required] then only
     * summarize its top level for token estimates and validation.
     */
    val schema: JsonObject? = null,
)

@Serializable
data class ToolProperty(
    val type: String,
    val description: String,
    /** For an array: the schema of one element. */
    val items: ToolProperty? = null,
    /** For an object: the schema of each field it carries. */
    val properties: Map<String, ToolProperty>? = null,
    /** For an object: which of its own fields are mandatory. */
    val required: List<String>? = null,
)

@Serializable
data class OpenAiChatRequest(
    val model: String,
    val messages: List<OpenAiMessage>,
    val stream: Boolean = true,
    @SerialName("stream_options") val streamOptions: OpenAiStreamOptions? = null,
    val tools: List<ToolDefinition>? = null,
    @SerialName("reasoning_effort") val reasoningEffort: String? = null,
    @SerialName("thinking") val thinking: OpenAiThinking? = null,
    @SerialName("enable_thinking") val enableThinking: Boolean? = null,
    @SerialName("thinking_budget") val thinkingBudget: Int? = null,
    val reasoning: OpenAiReasoning? = null,
    /** Groq: `false` returns no reasoning for a model that cannot turn reasoning off. */
    @SerialName("include_reasoning") val includeReasoning: Boolean? = null,
    val plugins: List<OpenAiPlugin>? = null,
    @SerialName("service_tier") val serviceTier: String? = null,
    val temperature: Float? = null,
    @SerialName("max_tokens") val maxTokens: Int? = null,
    @SerialName("top_p") val topP: Float? = null,
    @SerialName("frequency_penalty") val frequencyPenalty: Float? = null,
    @SerialName("presence_penalty") val presencePenalty: Float? = null,
    @SerialName("prompt_cache_key") val promptCacheKey: String? = null,
)

@Serializable
data class OpenAiPlugin(
    val id: String
)

/** DeepSeek thinking toggle: `{"thinking": {"type": "enabled" | "disabled"}}`. */
@Serializable
data class OpenAiThinking(
    val type: String,
)

@Serializable
data class OpenAiReasoning(
    val effort: String? = null,
    val summary: String? = null,
    @SerialName("max_tokens") val maxTokens: Int? = null,
    val enabled: Boolean? = null
)

@Serializable
data class OpenAiStreamOptions(
    @SerialName("include_usage") val includeUsage: Boolean = true
)

@Serializable
data class OpenAiMessage(
    val role: String,
    val content: List<OpenAiContentPart>? = null,
    @SerialName("tool_calls") val toolCalls: List<OpenAiRequestToolCall>? = null,
    @SerialName("tool_call_id") val toolCallId: String? = null,
    @SerialName("reasoning_content") val reasoningContent: String? = null,
    /** Provider-scoped raw Responses output items restored only by the Responses transport. */
    @Transient val responseOutputItems: List<JsonObject>? = null,
    @Transient val responseOutputItemProvider: String? = null,
    /** Model that produced [responseOutputItems]; another model cannot read them back. */
    @Transient val responseOutputItemModel: String? = null,
)

@Serializable
data class OpenAiRequestToolCall(
    val id: String,
    val type: String = "function",
    val function: OpenAiRequestFunction
)

@Serializable
data class OpenAiRequestFunction(
    val name: String,
    val arguments: String
)

@Serializable
data class OpenAiContentPart(
    val type: String,
    val text: String? = null,
    @SerialName("image_url") val imageUrl: OpenAiImageUrl? = null
)

@Serializable
data class OpenAiImageUrl(
    val url: String
)
