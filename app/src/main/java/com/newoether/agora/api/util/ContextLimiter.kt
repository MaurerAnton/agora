package com.newoether.agora.api.util

import com.newoether.agora.api.util.tokens.ContextCostModel
import com.newoether.agora.api.util.tokens.CostModelContextEstimator
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.Participant

/**
 * Context-window trimming: keeps the newest protocol-complete suffix that fits [contextTokenBudget]
 * under the model's own cost model. Lives separately from the wire conversion in MessageConverter.kt
 * because it depends on the token-estimation subtree; the wire golden stand compiles the converter
 * without it.
 */
fun limitContext(
    messages: List<ChatMessage>,
    contextTokenBudget: Int,
    includeAssistantReasoning: Boolean = false,
    costs: ContextCostModel = ContextCostModel.Default,
): List<ChatMessage> {
    if (messages.isEmpty()) return emptyList()
    val estimator = CostModelContextEstimator(costs)

    // A tool call and all of its results are one protocol unit. Truncating the flat list can leave
    // either an orphan result or an unanswered assistant tool call, so window complete units only.
    val units = protocolAtomicUnits(messages)

    val selected = ArrayDeque<List<ChatMessage>>()
    var estimatedTokens = 0L
    var hasNormalUserAnchor = false
    val tokenBudget = contextTokenBudget.coerceAtLeast(1).toLong()
    for (unit in units.asReversed()) {
        val unitCost = estimator.estimate(
            unit,
            includeAssistantReasoning = includeAssistantReasoning,
        ).toLong()
        if (
            selected.isNotEmpty() &&
            hasNormalUserAnchor &&
            estimatedTokens + unitCost > tokenBudget
        ) break
        selected.addFirst(unit)
        estimatedTokens = (estimatedTokens + unitCost).coerceAtMost(Int.MAX_VALUE.toLong())
        if (unit.any {
                it.participant == Participant.USER && !it.isToolProtocolMessage()
            }
        ) hasNormalUserAnchor = true
        // Always retain a legal user anchor and at least the newest complete protocol unit, even
        // when that single input already exceeds the configured estimate.
        if (hasNormalUserAnchor && estimatedTokens >= tokenBudget) break
    }

    val flattened = selected.flatten()
    // All supported chat protocols accept a user-led suffix. Starting at an assistant/tool turn
    // after truncation is ambiguous and is a common source of provider-side 400 responses.
    val firstNormalUser = flattened.indexOfFirst {
        it.participant == Participant.USER && !it.isToolProtocolMessage()
    }
    return if (firstNormalUser >= 0) flattened.drop(firstNormalUser) else emptyList()
}
