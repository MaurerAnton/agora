package com.newoether.agora.api.openai

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.OpenAiPlugin
import com.newoether.agora.api.OpenAiReasoning
import com.newoether.agora.api.OpenAiThinking
import com.newoether.agora.model.ResolvedThinking

/**
 * The provider-specific `customizeRequest` bodies. They live outside the provider classes so the
 * golden harness under `core/tools/golden/openai-request` can pin them without pulling in the HTTP
 * runtime; every provider method delegates here, and the C++ port owns the same shapes.
 */

/**
 * OpenAI: Chat Completions `reasoning_effort` accepts none/minimal/low/medium/high/xhigh/max.
 * "none" is how the API expresses thinking off, and a model documented without an effort selector
 * simply omits the field.
 */
internal fun OpenAiChatRequest.withOpenAiThinking(resolved: ResolvedThinking): OpenAiChatRequest {
    val effort = if (resolved.disabled) {
        "none".takeIf { resolved.capability.supportsEffort }
    } else {
        resolved.effort
    }
    return if (effort == null) this else copy(reasoningEffort = effort)
}

/**
 * Applies the DeepSeek thinking controls to an OpenAI-format request. `thinking.type` toggles
 * thinking and `reasoning_effort` carries the level the model accepts.
 */
internal fun OpenAiChatRequest.withDeepSeekThinking(
    resolved: ResolvedThinking,
): OpenAiChatRequest = if (resolved.disabled) {
    copy(thinking = OpenAiThinking(type = "disabled"))
} else {
    copy(
        thinking = OpenAiThinking(type = "enabled"),
        reasoningEffort = resolved.effort,
    )
}

/**
 * Groq's `reasoning_effort` accepts low/medium/high, plus none/default on the models documented
 * for them. A model that cannot turn reasoning off keeps its lowest effort and hides the
 * reasoning with `include_reasoning: false` instead of failing the request.
 */
internal fun OpenAiChatRequest.withGroqThinking(
    resolved: ResolvedThinking,
    thinkingEnabled: Boolean,
): OpenAiChatRequest {
    val capability = resolved.capability
    if (!thinkingEnabled && !capability.canDisableThinking) {
        return copy(
            reasoningEffort = resolved.effort,
            includeReasoning = false,
        )
    }
    val effort = if (resolved.disabled) {
        "none".takeIf { capability.supportsEffort }
    } else {
        resolved.effort
    }
    return copy(reasoningEffort = effort)
}

/**
 * DashScope's compatible mode has two documented thinking shapes, and the model's capability
 * entry decides which one applies:
 *  - effort models take `reasoning_effort` (with `none` turning thinking off) and reject
 *    `reasoning_effort` together with `thinking_budget`;
 *  - hybrid models take `enable_thinking` plus an optional `thinking_budget`.
 */
internal fun OpenAiChatRequest.withQwenThinking(resolved: ResolvedThinking): OpenAiChatRequest =
    if (resolved.capability.supportsEffort) {
        copy(
            reasoningEffort = if (resolved.disabled) "none" else resolved.effort,
            thinkingBudget = resolved.budgetTokens,
        )
    } else {
        copy(
            enableThinking = resolved.enabled,
            thinkingBudget = resolved.budgetTokens,
        )
    }

/**
 * OpenRouter takes `reasoning.effort` (minimal..max) or `reasoning.max_tokens`, never both, and
 * `reasoning.enabled = false` turns thinking off. The accepted set comes from the model's
 * capability entry rather than a name test.
 */
internal fun OpenAiChatRequest.withOpenRouterReasoning(
    resolved: ResolvedThinking,
    googleSearchEnabled: Boolean,
): OpenAiChatRequest {
    val reasoning = if (resolved.disabled) {
        OpenAiReasoning(enabled = false)
    } else {
        OpenAiReasoning(effort = resolved.effort, maxTokens = resolved.budgetTokens)
    }
    return copy(
        reasoning = reasoning,
        plugins = if (googleSearchEnabled) listOf(OpenAiPlugin(id = "web")) else null
    )
}

/**
 * A relay has no published parameter documentation, so no model id may decide the request shape
 * here; thinking settings are forwarded as the capability resolves them. CheapestInference
 * documents only none/low/medium/high and no budget field, so its requests are narrowed to that
 * shape (https://cheapestinference.com/docs/api/reasoning/).
 */
internal fun OpenAiChatRequest.withCustomOpenAiThinking(
    resolved: ResolvedThinking,
    defaultBaseUrl: String,
): OpenAiChatRequest {
    if (isCheapestInferenceEndpoint(defaultBaseUrl)) {
        return copy(
            reasoningEffort = cheapestInferenceEffort(
                disabled = resolved.disabled,
                effort = resolved.effort,
            ),
            thinkingBudget = null,
        )
    }
    val effort = if (resolved.disabled) {
        "none".takeIf { resolved.capability.supportsEffort }
    } else {
        resolved.effort
    }
    return copy(
        reasoningEffort = effort,
        thinkingBudget = resolved.budgetTokens,
    )
}

internal fun isCheapestInferenceEndpoint(defaultBaseUrl: String): Boolean =
    defaultBaseUrl.contains("cheapestinference.com", ignoreCase = true)

internal fun cheapestInferenceEffort(disabled: Boolean, effort: String?): String {
    if (disabled) return "none"
    return when (effort) {
        "none", "minimal", "low" -> "low"
        "medium" -> "medium"
        else -> "high"
    }
}
