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
     * A relay has no published parameter documentation, so no model id may decide the request
     * shape here. Thinking settings are forwarded exactly as the user configured them through the
     * permissive default capability, which the user can narrow per model when a relay rejects a
     * field; the shape itself lives in [withCustomOpenAiThinking].
     */
    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest = request.withCustomOpenAiThinking(
        config.resolvedThinking(ThinkingProviderFamily.OPENAI_COMPATIBLE),
        defaultBaseUrl,
    )

    /** Same rule as the built-in provider: a relayed DeepSeek model replays chain of thought. */
    override fun forwardsAssistantReasoningContent(config: ProviderConfig): Boolean =
        config.includeAssistantReasoning
}
