package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.OpenAiMessage
import com.newoether.agora.api.OpenAiStreamOptions
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.ProviderRequestInput
import com.newoether.agora.api.util.Base64FileRegistry
import com.newoether.agora.api.util.StreamingJsonBody
import com.newoether.agora.api.util.convertToOpenAiMessages
import com.newoether.agora.api.util.requireValidSerializedRequest
import kotlinx.serialization.json.Json

/**
 * The serialized chat-completions body plus everything the transport needs to open it.
 * [streamingBody] is non-null only when at least one image file was projected as a placeholder.
 */
internal data class OpenAiBuiltRequest(
    val apiMessages: List<OpenAiMessage>,
    val bodyJson: String,
    val streamingBody: StreamingJsonBody?,
)

/**
 * Provider-neutral chat-completions request pipeline:
 *
 *   conversation -> wire messages -> typed request -> customize hook -> object validation ->
 *   kotlinx serialization -> serialized-body gate -> base64 body preparation.
 *
 * The provider supplies its identity and its override points as functions; the step order and
 * every wire shape are pinned byte for byte by the golden traces under
 * `core/tools/golden/openai-request` and must stay identical in the C++ port.
 */
internal class OpenAiChatRequestFactory(
    private val providerName: String,
    private val json: Json = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false },
    private val customizeRequest: (OpenAiChatRequest, ProviderConfig) -> OpenAiChatRequest = { request, _ -> request },
    private val transformSystemPrompt: (String?) -> String? = { it },
    private val forwardsAssistantReasoning: (ProviderConfig) -> Boolean = { false },
) {
    fun build(config: ProviderConfig, input: ProviderRequestInput): OpenAiBuiltRequest {
        val base64Files = Base64FileRegistry()
        val apiMessages = convertToOpenAiMessages(
            messages = input.messages,
            systemPrompt = transformSystemPrompt(input.systemPrompt),
            includeImages = config.includeImages,
            base64Files = base64Files,
            forwardAssistantReasoning = forwardsAssistantReasoning(config),
        )
        var request = OpenAiChatRequest(
            model = config.modelId,
            messages = apiMessages,
            stream = true,
            streamOptions = OpenAiStreamOptions(includeUsage = true),
            serviceTier = config.openAiServiceTier,
            tools = config.tools,
            temperature = config.temperature,
            maxTokens = config.maxTokens,
            topP = config.topP,
            frequencyPenalty = config.frequencyPenalty,
            presencePenalty = config.presencePenalty,
            promptCacheKey = config.promptCacheKey,
        )
        request = customizeRequest(request, config)
        request.requireValidWireFormat(providerName)
        val bodyJson = json.encodeToString(OpenAiChatRequest.serializer(), request)
        requireValidSerializedRequest(
            provider = providerName,
            body = bodyJson,
            requiredStringFields = setOf("model"),
            requiredArrayFields = setOf("messages"),
        )
        return OpenAiBuiltRequest(
            apiMessages = apiMessages,
            bodyJson = bodyJson,
            streamingBody = base64Files.prepare(bodyJson),
        )
    }
}
