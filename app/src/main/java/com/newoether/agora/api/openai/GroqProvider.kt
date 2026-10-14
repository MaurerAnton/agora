package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.model.ThinkingLevels
import com.newoether.agora.util.Constants

class GroqProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_GROQ
    override val defaultBaseUrl: String = "https://api.groq.com/openai/v1"

    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest {
        val model = config.modelId.trim().lowercase()
        val level = ThinkingLevels.normalize(config.thinkingLevel)
        val effort = when (model) {
            "qwen/qwen3.6-27b" ->
                if (!config.thinkingEnabled || level == "none") "none" else "default"
            "qwen/qwen3.8-27b" -> when {
                !config.thinkingEnabled || level == "none" -> "none"
                level in setOf("minimal", "low") -> "low"
                level == "medium" -> "medium"
                else -> "high"
            }
            "openai/gpt-oss-20b", "openai/gpt-oss-120b" -> {
                if (!config.thinkingEnabled || level == "none") {
                    throw RequestFormatException(
                        name,
                        listOf("model ${config.modelId} cannot disable reasoning"),
                    )
                }
                when (level) {
                    "minimal", "low" -> "low"
                    "medium" -> "medium"
                    else -> "high"
                }
            }
            else -> null
        }
        return request.copy(reasoningEffort = effort)
    }
}
