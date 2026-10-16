package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.util.Constants

class GroqProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_GROQ
    override val defaultBaseUrl: String = "https://api.groq.com/openai/v1"

    /**
     * Groq's `reasoning_effort` accepts low/medium/high, plus none/default on the models documented
     * for them. The reasoning shape lives in [withGroqThinking].
     */
    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest = request.withGroqThinking(
        config.resolvedThinking(ThinkingProviderFamily.GROQ),
        config.thinkingEnabled,
    )
}
