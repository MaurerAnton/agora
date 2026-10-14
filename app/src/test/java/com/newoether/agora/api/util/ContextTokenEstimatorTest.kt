package com.newoether.agora.api.util

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.Participant
import com.newoether.agora.util.Constants
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty

class ContextTokenEstimatorTest {

    @Test
    fun fixedCostIncludesSystemPromptAndCompleteToolSchemaDeterministically() {
        val tool = ToolDefinition(
            function = ToolFunction(
                name = "shell",
                description = "Execute a command",
                parameters = ToolParameters(
                    properties = linkedMapOf(
                        "timeout" to ToolProperty("integer", "Timeout seconds"),
                        "command" to ToolProperty("string", "Command text"),
                    ),
                    required = listOf("command"),
                ),
            ),
        )

        val first = ContextTokenEstimator.estimateFixed("System prompt", listOf(tool))
        val reordered = tool.copy(
            function = tool.function.copy(
                parameters = tool.function.parameters.copy(
                    properties = tool.function.parameters.properties.entries
                        .reversed()
                        .associate { it.toPair() },
                ),
            ),
        )

        assertEquals(first, ContextTokenEstimator.estimateFixed("System prompt", listOf(reordered)))
        assertTrue(first > ContextTokenEstimator.estimateFixed(null, emptyList()))
    }

    @Test
    fun fixedCostIncludesApiOnlyInitialUserPrompt() {
        val withoutInvocation = ContextTokenEstimator.estimateFixed(
            systemPrompt = "System prompt",
            tools = emptyList(),
        )
        val withInvocation = ContextTokenEstimator.estimateFixed(
            systemPrompt = "System prompt",
            tools = emptyList(),
            initialUserPrompt = "Create the compact context summary now.",
        )

        assertTrue(withInvocation > withoutInvocation)
    }

    @Test
    fun fixedCostIncludesEnabledProviderNativeTools() {
        val baseline = ContextTokenEstimator.estimateFixed(null, emptyList())
        val enabled = ContextTokenEstimator.estimateFixed(
            systemPrompt = null,
            tools = emptyList(),
            codeExecutionEnabled = true,
            googleSearchEnabled = true,
            openAiWebSearchEnabled = true,
        )

        assertTrue(enabled > baseline)
    }

    @Test
    fun multilingualTextIsDeterministicAndNonZero() {
        val text = "hello world 你好，世界 👋"
        val first = ContextTokenEstimator.estimateText(text)

        assertTrue(first >= 10)
        assertEquals(first, ContextTokenEstimator.estimateText(text))
    }

    @Test
    fun toolArgumentsAndResultsContributeToCost() {
        val plain = message("u", "start", Participant.USER)
        val tool = message(Constants.TOOL_MSG_PREFIX + "1", "", Participant.MODEL).copy(
            segments = listOf(
                MessageSegment(
                    type = "tool",
                    toolName = "file_read",
                    toolArgs = """{"path":"/a/very/long/path"}""",
                )
            )
        )
        val result = message(Constants.RESULT_MSG_PREFIX + "1", "", Participant.USER).copy(
            segments = listOf(
                MessageSegment(
                    type = "tool",
                    toolName = "file_read",
                    toolArgs = "{}",
                    toolResult = "result ".repeat(100),
                )
            )
        )

        assertTrue(
            ContextTokenEstimator.estimate(listOf(plain, tool, result)) >
                ContextTokenEstimator.estimate(listOf(plain))
        )
    }

    @Test
    fun mirroredToolResultTextIsCountedOnlyThroughItsWirePayload() {
        val segment = MessageSegment(
            type = "tool",
            toolName = "file_read",
            toolArgs = "{}",
            toolResult = "provider-visible result",
            toolCallId = "call-1",
        )
        val withMirroredRoomText = message(
            Constants.RESULT_MSG_PREFIX + "1",
            "provider-visible result",
            Participant.USER,
        ).copy(segments = listOf(segment))
        val withoutMirroredRoomText = withMirroredRoomText.copy(text = "")

        assertEquals(
            ContextTokenEstimator.estimate(listOf(withoutMirroredRoomText)),
            ContextTokenEstimator.estimate(listOf(withMirroredRoomText)),
        )
    }

    @Test
    fun toolReasoningAndOpaqueContinuationContributeToCost() {
        val baseline = message(Constants.TOOL_MSG_PREFIX + "1", "", Participant.MODEL).copy(
            segments = listOf(
                MessageSegment(
                    type = "tool",
                    toolName = "shell",
                    toolArgs = "{}",
                ),
            ),
        )
        val continuation = Json.parseToJsonElement(
            """{"type":"reasoning","encrypted_content":"opaque-state"}""",
        ).jsonObject
        val complete = baseline.copy(
            segments = listOf(
                MessageSegment(type = "thought", content = "provider-visible reasoning"),
                MessageSegment(
                    type = "tool",
                    toolName = "shell",
                    toolArgs = "{}",
                    responseOutputItems = listOf(continuation),
                ),
            ),
        )

        assertTrue(
            ContextTokenEstimator.estimate(listOf(complete)) >
                ContextTokenEstimator.estimate(listOf(baseline)),
        )
    }

    @Test
    fun everyProviderVisibleReasoningBlockAndSignatureContributesToCost() {
        val tool = MessageSegment(
            type = "tool",
            toolName = "shell",
            toolArgs = "{}",
        )
        val lastOnly = message(
            Constants.TOOL_MSG_PREFIX + "reasoning",
            "",
            Participant.MODEL,
        ).copy(
            segments = listOf(
                MessageSegment(
                    type = "thought",
                    content = "second reasoning block ".repeat(8),
                    signature = "second-signature",
                ),
                tool,
            ),
        )
        val complete = lastOnly.copy(
            segments = listOf(
                MessageSegment(
                    type = "thought",
                    content = "first reasoning block ".repeat(8),
                    signature = "first-signature",
                ),
            ) + lastOnly.segments.orEmpty(),
        )

        assertTrue(
            ContextTokenEstimator.estimate(listOf(complete)) >
                ContextTokenEstimator.estimate(listOf(lastOnly)),
        )
    }

    @Test
    fun everyProjectedImageContributesToTheEstimate() {
        val oneImage = message("u", "attachment", Participant.USER).copy(
            images = listOf("page-1.jpg"),
        )
        val threeImages = oneImage.copy(
            images = listOf("page-1.jpg", "page-2.jpg", "video-frame.jpg"),
        )

        assertTrue(
            ContextTokenEstimator.estimate(listOf(threeImages)) >
                ContextTokenEstimator.estimate(listOf(oneImage)),
        )
    }

    @Test
    fun apiOnlyInitialPromptDoesNotConsumeTheDurableHistoryWindow() {
        val history = listOf(
            message("older-user", "older ".repeat(40), Participant.USER),
            message("older-model", "answer ".repeat(20), Participant.MODEL),
            message("latest-user", "latest", Participant.USER),
        )
        val prompt = message(
            "api_initial_user_latest-user",
            "Create the compact summary now.",
            Participant.USER,
        ).copy(parentId = "latest-user")
        val expectedHistory = prepareMessages(history, contextTokenBudget = 80)
        val prepared = prepareMessages(history + prompt, contextTokenBudget = 80)

        assertEquals(expectedHistory.map { it.id } + prompt.id, prepared.map { it.id })
        assertEquals(prompt, prepared.last())
    }

    @Test
    fun durablePrefixCollisionRemainsOrdinaryHistory() {
        val durable = message(
            "api_initial_user_imported",
            "ordinary durable message",
            Participant.USER,
        )
        val prepared = prepareMessages(listOf(durable), contextTokenBudget = 1)

        assertEquals(listOf(durable), prepared)
    }

    @Test
    fun contextLimitNeverSplitsLatestToolRoundAndRetainsUserAnchor() {
        val user = message("u", "start", Participant.USER)
        val tool = message(Constants.TOOL_MSG_PREFIX + "1", "", Participant.MODEL)
        val result = message(Constants.RESULT_MSG_PREFIX + "1", "large ".repeat(100), Participant.USER)

        assertEquals(
            listOf("u", tool.id, result.id),
            limitContext(listOf(user, tool, result), contextTokenBudget = 1).map { it.id },
        )
    }

    private fun message(id: String, text: String, participant: Participant) = ChatMessage(
        id = id,
        text = text,
        participant = participant,
    )
}
