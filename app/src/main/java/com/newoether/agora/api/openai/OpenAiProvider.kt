package com.newoether.agora.api.openai

import com.newoether.agora.api.*
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.util.Constants

class OpenAiProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_OPENAI
    override val defaultBaseUrl: String = "https://api.openai.com/v1"

    /**
     * Chat Completions `reasoning_effort` accepts none/minimal/low/medium/high/xhigh/max. The
     * value comes from the model's capability entry, not from a model-name test.
     */
    override fun customizeRequest(request: OpenAiChatRequest, config: ProviderConfig): OpenAiChatRequest =
        request.withOpenAiThinking(config.resolvedThinking(ThinkingProviderFamily.OPENAI))
    // Reasoning/content parsing uses BaseOpenAiProvider's default (reasoning_content + content).
}
