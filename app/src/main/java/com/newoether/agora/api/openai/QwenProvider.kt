package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.util.Constants

class QwenProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_QWEN
    override val defaultBaseUrl: String = "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"

    override fun customizeRequest(
        request: OpenAiChatRequest,
        config: ProviderConfig,
    ): OpenAiChatRequest {
        val model = config.modelId.trim().lowercase()
        if (isQwen38EffortModel(model)) {
            if (!config.thinkingEnabled) return request.copy(reasoningEffort = "none")
            val budget = qwenThinkingBudget(config, supported = true)
            return if (budget != null) {
                request.copy(thinkingBudget = budget)
            } else {
                request.copy(
                    reasoningEffort = qwen38ReasoningEffort(
                        modelId = model,
                        thinkingEnabled = true,
                        thinkingLevel = config.thinkingLevel,
                    ),
                )
            }
        }
        if (isQwenThinkingOnlyModel(model)) {
            if (!config.thinkingEnabled) {
                throw RequestFormatException(
                    name,
                    listOf("model ${config.modelId} cannot disable thinking"),
                )
            }
            return request.copy(
                thinkingBudget = qwenThinkingBudget(
                    config,
                    supported = model.startsWith("qwen3"),
                ),
            )
        }
        if (!isQwenHybridThinkingModel(model)) return request

        return request.copy(
            enableThinking = config.thinkingEnabled,
            thinkingBudget = qwenThinkingBudget(
                config,
                supported = !model.startsWith("qwen3-omni"),
            ),
        )
    }

    // Reasoning/content parsing uses BaseOpenAiProvider's default (reasoning_content + content).
}

private fun qwenThinkingBudget(config: ProviderConfig, supported: Boolean): Int? {
    if (!supported || !config.thinkingEnabled || !config.thinkingBudgetEnabled) return null
    if (config.thinkingBudgetTokens <= 0) {
        throw RequestFormatException(
            Constants.PROVIDER_QWEN,
            listOf("thinking_budget must be positive"),
        )
    }
    return config.thinkingBudgetTokens
}

private fun isQwen38EffortModel(model: String): Boolean =
    model == "qwen3.8-max" || model.startsWith("qwen3.8-max-") ||
        model == "qwen3.8-flash" || model.startsWith("qwen3.8-flash-")

private val qwenThinkingOnlyModels = setOf(
    "qwen3.7-max-preview", "qwen3.7-max-2026-05-17", "qwen3-next-80b-a3b-thinking",
    "qwen3-235b-a22b-thinking-2507", "qwen3-30b-a3b-thinking-2507", "qwq-plus",
)

private fun isQwenThinkingOnlyModel(model: String): Boolean =
    model in qwenThinkingOnlyModels || model.startsWith("qwq-plus-")

private val qwenHybridThinkingModels = setOf(
    "qwen3.7-max", "qwen3.7-max-us", "qwen3.7-max-2026-05-20", "qwen3.7-max-2026-06-08",
    "qwen3.7-plus", "qwen3.7-plus-us", "qwen3.7-plus-2026-05-26",
    "qwen3.7-flash", "qwen3.7-flash-2026-07-15", "qwen3.6-max-preview",
    "qwen3.6-plus", "qwen3.6-plus-2026-04-02", "qwen3.6-flash", "qwen3.6-flash-2026-04-16",
    "qwen3.6-35b-a3b", "qwen3.5-plus", "qwen3.5-plus-2026-02-15",
    "qwen3.5-flash", "qwen3.5-flash-2026-02-23", "qwen3.5-397b-a17b", "qwen3.5-122b-a10b",
    "qwen3.5-27b", "qwen3.5-35b-a3b", "qwen3-max", "qwen3-max-2026-01-23", "qwen3-max-preview",
    "qwen-plus", "qwen-plus-latest", "qwen-flash", "qwen-turbo",
    "qwen3-235b-a22b", "qwen3-32b", "qwen3-30b-a3b", "qwen3-14b", "qwen3-8b",
)

private fun isQwenHybridThinkingModel(model: String): Boolean = when {
    model in qwenHybridThinkingModels -> true
    model.startsWith("qwen-plus-20") || model.startsWith("qwen-flash-20") -> true
    model.startsWith("qwen-turbo-") -> true
    model.startsWith("qwen3-vl-") || model.startsWith("qwen3-omni-flash") -> true
    else -> false
}
