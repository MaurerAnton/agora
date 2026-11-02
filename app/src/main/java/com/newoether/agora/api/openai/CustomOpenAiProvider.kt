package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.model.ThinkingProviderFamily

class CustomOpenAiProvider(
    override val name: String,
    override val defaultBaseUrl: String
) : BaseOpenAiProvider() {

    override val retryableStatusCodes: Set<Int> = setOf(429, 502, 503, 504)

    override val retryMissingV1BaseUrl: Boolean = true

    /**
     * A relay publishes no parameter documentation of its own, so the request shape comes from
     * the shared capability resolution (documented tables, nearest accepted effort) plus what
     * [withCustomOpenAiThinking] adds: the Qwen3.8 template controls a model genuinely needs and
     * the CheapestInference narrowing of fields that endpoint rejects.
     */
    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest = request.withCustomOpenAiThinking(
        config.resolvedThinking(ThinkingProviderFamily.OPENAI_COMPATIBLE),
        defaultBaseUrl,
        config.modelId,
    )

    /** Same rule as the built-in provider: a relayed DeepSeek model replays chain of thought. */
    override fun forwardsAssistantReasoningContent(config: ProviderConfig): Boolean =
        config.includeAssistantReasoning
}
