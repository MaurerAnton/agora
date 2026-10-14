package com.newoether.agora.api

import com.newoether.agora.api.util.StreamingThinkTagParser

class CustomOpenAiProvider(
    override val name: String,
    override val defaultBaseUrl: String
) : BaseOpenAiProvider() {

    override fun customizeRequest(request: OpenAiChatRequest, config: ProviderConfig): OpenAiChatRequest {
        if (!isCheapestInferenceEndpoint()) return request
        // CheapestInference serves reasoning off by default; reasoning_effort opts in
        // (low/medium/high) and "none" disables it. UI levels already match that scale.
        // https://cheapestinference.com/docs/api/reasoning/
        return request.copy(
            reasoningEffort = if (config.thinkingEnabled) config.thinkingLevel else "none",
        )
    }

    private fun isCheapestInferenceEndpoint(): Boolean =
        defaultBaseUrl.contains("cheapestinference.com", ignoreCase = true)

    override suspend fun parseDeltaContent(
        delta: OpenAiDelta,
        config: ProviderConfig,
        thinkParser: StreamingThinkTagParser,
        emit: suspend (StreamEvent) -> Unit
    ) {
        delta.reasoningContent?.let { reasoning ->
            if (reasoning.isNotEmpty() && config.thinkingEnabled) {
                emit(StreamEvent.ThoughtChunk(reasoning))
            }
        }
        delta.content?.let { content ->
            if (content.isNotEmpty()) emit(StreamEvent.TextChunk(content))
        }
    }
}
