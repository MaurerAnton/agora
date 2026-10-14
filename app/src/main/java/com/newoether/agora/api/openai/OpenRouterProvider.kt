package com.newoether.agora.api.openai

import com.newoether.agora.api.*
import com.newoether.agora.model.ThinkingLevels
import com.newoether.agora.util.Constants

private val BOLD_TITLE_REGEX = Regex("\\*\\*(.*?)\\*\\*")
private val HEADING_TITLE_REGEX = Regex("(?m)^#+\\s*(.*)$")

/** First bold (`**...**`) or markdown-heading line in a reasoning chunk, used as its title. */
private fun extractThoughtTitle(text: String): String? =
    BOLD_TITLE_REGEX.find(text)?.groupValues?.get(1)
        ?: HEADING_TITLE_REGEX.find(text)?.groupValues?.get(1)

class OpenRouterProvider : BaseOpenAiProvider() {
    override val name: String = Constants.PROVIDER_OPEN_ROUTER
    override val defaultBaseUrl: String = "https://openrouter.ai/api/v1"

    override fun customizeRequest(request: OpenAiChatRequest, config: ProviderConfig): OpenAiChatRequest {
        val reasoning = if (config.thinkingEnabled) {
            OpenAiReasoning(
                effort = if (!config.thinkingBudgetEnabled) ThinkingLevels.openRouterEffort(config.thinkingLevel) else null,
                maxTokens = config.thinkingBudgetTokens.takeIf { config.thinkingBudgetEnabled }
            )
        } else null
        return request.copy(
            reasoning = reasoning,
            plugins = if (config.googleSearchEnabled) listOf(OpenAiPlugin(id = "web")) else null
        )
    }

    override fun getExtraHeaders(config: ProviderConfig): Map<String, String> = mapOf(
        "HTTP-Referer" to "https://github.com/newo-ether/Agora",
        "X-Title" to "Agora"
    )

    override suspend fun parseDeltaContent(
        delta: OpenAiDelta,
        config: ProviderConfig,
        emit: suspend (StreamEvent) -> Unit
    ) {
        val hasEffectiveStructuredReasoning = delta.reasoningDetails.orEmpty().any { detail ->
            (detail.type == "reasoning.text" || detail.type == "text") &&
                !detail.text.isNullOrBlank()
        }
        delta.reasoningDetails?.forEach { detail ->
            if (detail.type == "reasoning.text" || detail.type == "text") {
                detail.text?.takeIf(String::isNotBlank)?.let {
                    emit(StreamEvent.ThoughtChunk(it, extractThoughtTitle(it)))
                }
            }
        }
        // Fall back to the bare `reasoning` string only when no effective structured text exists;
        // OpenRouter can flatten the same text into both, so an unconditional read would duplicate it.
        val fallbackReasoning = delta.reasoningContent?.takeIf(String::isNotBlank)
            ?: delta.reasoning?.takeIf(String::isNotBlank)
        fallbackReasoning
            ?.takeIf { !hasEffectiveStructuredReasoning }
            ?.let {
                emit(StreamEvent.ThoughtChunk(it, extractThoughtTitle(it)))
            }
        delta.content?.let { content ->
            if (content.isNotEmpty()) emit(StreamEvent.TextChunk(content))
        }
    }
}
