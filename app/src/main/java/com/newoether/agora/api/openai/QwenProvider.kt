package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.util.Constants

class QwenProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_QWEN
    override val defaultBaseUrl: String = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"

    /**
     * DashScope's compatible mode has two documented thinking shapes, and the model's capability
     * entry decides which one applies. The shapes live in [withQwenThinking].
     */
    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest =
        request.withQwenThinking(config.resolvedThinking(ThinkingProviderFamily.QWEN))

    // Reasoning/content parsing uses BaseOpenAiProvider's default (reasoning_content + content).
}
