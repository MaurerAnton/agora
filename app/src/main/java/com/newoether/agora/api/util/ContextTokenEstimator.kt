package com.newoether.agora.api.util

import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.Participant
import com.newoether.agora.util.Constants
import kotlin.math.ceil

/**
 * Deterministic cross-provider estimate of Provider-visible conversation tokens.
 *
 * Exact tokenization is model-specific and unavailable offline for arbitrary custom providers.
 * This estimator intentionally leans conservative: ASCII word-like runs use roughly four
 * characters per token, non-ASCII code points and punctuation cost one each, and message/image/
 * tool framing has explicit overhead. Every threshold, rollout decision, and UI indicator uses
 * this same function, so the estimate cannot drift between surfaces.
 */
object ContextTokenEstimator {
    private const val MESSAGE_OVERHEAD = 8
    private const val IMAGE_ESTIMATE = 1_024
    private const val TOOL_CALL_OVERHEAD = 16
    private const val SAFETY_NUMERATOR = 11L
    private const val SAFETY_DENOMINATOR = 10L

    fun estimate(messages: List<ChatMessage>): Int {
        val raw = messages.fold(0L) { total, message ->
            (total + estimateMessageRaw(message)).coerceAtMost(Int.MAX_VALUE.toLong())
        }
        return applySafetyMargin(raw)
    }

    /** Provider-visible cost that exists even when the conversation history is empty. */
    fun estimateFixed(
        systemPrompt: String?,
        tools: List<ToolDefinition>,
        initialUserPrompt: String? = null,
        codeExecutionEnabled: Boolean = false,
        googleSearchEnabled: Boolean = false,
        openAiWebSearchEnabled: Boolean = false,
    ): Int {
        var raw = MESSAGE_OVERHEAD.toLong() + estimateTextRaw(systemPrompt.orEmpty())
        initialUserPrompt?.takeIf(String::isNotBlank)?.let { prompt ->
            raw += MESSAGE_OVERHEAD + estimateTextRaw(prompt)
        }
        tools.forEach { tool ->
            raw += TOOL_CALL_OVERHEAD
            raw += estimateTextRaw(tool.type)
            raw += estimateTextRaw(tool.function.name)
            raw += estimateTextRaw(tool.function.description)
            raw += estimateTextRaw(tool.function.parameters.type)
            tool.function.parameters.properties.toSortedMap().forEach { (name, property) ->
                raw += estimateTextRaw(name)
                raw += estimateToolPropertyRaw(property)
            }
            tool.function.parameters.required.sorted().forEach { required ->
                raw += estimateTextRaw(required)
            }
        }
        listOfNotNull(
            "code_execution".takeIf { codeExecutionEnabled },
            "google_search".takeIf { googleSearchEnabled },
            "web_search".takeIf { openAiWebSearchEnabled },
        ).forEach { nativeTool ->
            raw += TOOL_CALL_OVERHEAD + estimateTextRaw(nativeTool)
        }
        return applySafetyMargin(raw.coerceAtMost(Int.MAX_VALUE.toLong()))
    }

    internal fun estimateText(text: String): Int = applySafetyMargin(estimateTextRaw(text))

    private fun estimateMessageRaw(message: ChatMessage): Long {
        val isToolProtocol = message.id.startsWith(Constants.TOOL_MSG_PREFIX) ||
            message.id.startsWith(Constants.RESULT_MSG_PREFIX)
        // Provider adapters serialize tool protocol payload from segments/toolCall and ignore the
        // mirrored Room text field. Counting both made result-heavy contexts look up to 2x larger.
        var total = MESSAGE_OVERHEAD.toLong() +
            if (isToolProtocol) 0L else estimateTextRaw(message.text)
        if (!isToolProtocol && message.participant == Participant.USER) {
            total += message.images.size.toLong() * IMAGE_ESTIMATE
        }
        if (isToolProtocol) {
            message.segments.orEmpty()
                .asSequence()
                .filter { it.type == "thought" }
                .forEach { segment ->
                    total += estimateTextRaw(segment.content)
                    total += estimateTextRaw(segment.signature.orEmpty())
                }
            val segments = message.segments.orEmpty().filter { it.type == "tool" }
            if (segments.isNotEmpty()) {
                segments.forEach { segment ->
                    total += TOOL_CALL_OVERHEAD
                    total += estimateTextRaw(segment.toolName.orEmpty())
                    total += estimateTextRaw(segment.toolArgs.orEmpty())
                    total += estimateTextRaw(segment.toolResult.orEmpty())
                    total += estimateTextRaw(segment.signature.orEmpty())
                }
                segments.firstOrNull { it.responseOutputItems.isNotEmpty() }
                    ?.responseOutputItems
                    .orEmpty()
                    .forEach { item ->
                        total += estimateTextRaw(item.toString())
                    }
            } else {
                message.toolCall?.let { call ->
                    total += TOOL_CALL_OVERHEAD
                    total += estimateTextRaw(call.toolName)
                    total += estimateTextRaw(call.arguments)
                    total += estimateTextRaw(call.result)
                    total += estimateTextRaw(call.signature.orEmpty())
                    call.responseOutputItems.forEach { item ->
                        total += estimateTextRaw(item.toString())
                    }
                }
            }
        }
        return total
    }

    private fun estimateTextRaw(text: String): Long {
        if (text.isEmpty()) return 0L
        var tokens = 0L
        var asciiRun = 0

        fun flushAsciiRun() {
            if (asciiRun > 0) {
                tokens += ceil(asciiRun / 4.0).toLong()
                asciiRun = 0
            }
        }

        var index = 0
        while (index < text.length) {
            val codePoint = text.codePointAt(index)
            when {
                codePoint <= 0x7f && Character.isLetterOrDigit(codePoint) -> asciiRun++
                Character.isWhitespace(codePoint) -> flushAsciiRun()
                else -> {
                    flushAsciiRun()
                    tokens++
                }
            }
            index += Character.charCount(codePoint)
        }
        flushAsciiRun()
        return tokens
    }

    private fun estimateToolPropertyRaw(property: com.newoether.agora.api.ToolProperty): Long =
        estimateTextRaw(property.type) +
            estimateTextRaw(property.description) +
            (property.items?.let(::estimateToolPropertyRaw) ?: 0L)

    private fun applySafetyMargin(raw: Long): Int =
        ((raw * SAFETY_NUMERATOR + SAFETY_DENOMINATOR - 1) / SAFETY_DENOMINATOR)
            .coerceIn(0L, Int.MAX_VALUE.toLong())
            .toInt()
}
