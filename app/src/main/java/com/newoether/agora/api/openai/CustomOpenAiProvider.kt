package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.model.ThinkingLevels

internal fun qwen38ReasoningEffort(
    modelId: String,
    thinkingEnabled: Boolean,
    thinkingLevel: String,
): String? {
    val modelName = modelId.trim().substringAfterLast('/')
    if (!modelName.startsWith("qwen3.8", ignoreCase = true)) return null
    if (!thinkingEnabled) return "none"

    return when (ThinkingLevels.normalize(thinkingLevel)) {
        "none" -> "none"
        "minimal", "low" -> "low"
        "medium" -> "medium"
        "high", "xhigh", "max" -> "xhigh"
        else -> "medium"
    }
}

class CustomOpenAiProvider(
    override val name: String,
    override val defaultBaseUrl: String
) : BaseOpenAiProvider() {

    override val retryableStatusCodes: Set<Int> = setOf(429, 502, 503, 504)

    override val retryMissingV1BaseUrl: Boolean = true

    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest {
        val effort = qwen38ReasoningEffort(
            modelId = config.modelId,
            thinkingEnabled = config.thinkingEnabled,
            thinkingLevel = config.thinkingLevel,
        )
        if (effort != null) return request.copy(reasoningEffort = effort)
        if (isCheapestInferenceEndpoint()) {
            // CheapestInference serves reasoning off by default; reasoning_effort
            // low/medium/high opts in, "none" disables it.
            // https://cheapestinference.com/docs/api/reasoning/
            return request.copy(reasoningEffort = cheapestInferenceEffort(config))
        }
        return request
    }

    private fun isCheapestInferenceEndpoint(): Boolean =
        defaultBaseUrl.contains("cheapestinference.com", ignoreCase = true)

    private fun cheapestInferenceEffort(config: ProviderConfig): String {
        if (!config.thinkingEnabled) return "none"
        return when (ThinkingLevels.normalize(config.thinkingLevel)) {
            "none" -> "none"
            "minimal", "low" -> "low"
            "medium" -> "medium"
            else -> "high"
        }
    }
}
