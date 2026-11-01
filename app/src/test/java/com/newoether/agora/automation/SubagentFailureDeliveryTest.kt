package com.newoether.agora.automation

import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.SubagentCompletion
import com.newoether.agora.model.SubagentSummary
import io.mockk.coEvery
import io.mockk.coVerify
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class SubagentFailureDeliveryTest {
    @Test
    fun clearedOverlayStillDeliversPersistedFailureToQueueWaitAndList() = runTest {
        val f = SubagentManagerTest.Fixture(backgroundScope)
        coEvery { f.engine.runOnceWithAutomationGuardsHeld(any(), any(), any(), any(), any(), any(), any(), any(), any()) } coAnswers {
            val child = firstArg<String>()
            f.terminalMessages["failed-answer"] = MessageEntity(
                id = "failed-answer", conversationId = child, text = "Partial progress",
                participant = Participant.MODEL, status = MessageStatus.ERROR, timestamp = 2L,
                runId = "failed-run", runSequence = 1L,
                toolCallJson = Json.encodeToString(listOf(
                    MessageSegment("answer", "Partial progress"),
                    MessageSegment("error", "Socket is closed", errorCode = "connection_closed"),
                )),
            )
            f.messages[child] = MessageEntity(
                id = "later-tool", conversationId = child, text = "Unrelated later row",
                participant = Participant.USER, timestamp = Long.MAX_VALUE,
                runId = "failed-run", runSequence = 2L,
            )
            assertNull(f.registry.get(child)?.streamingMessage?.value)
            TaskExecutionEngine.Result.Failure("Partial progress", "failed-answer")
        }
        val child = f.spawn()
        runCurrent()
        val expected = SubagentCompletion("failed-answer", "failed", "Partial progress", "failed-answer",
            errorText = "Socket is closed", errorCode = "connection_closed")
        assertEquals(expected, SubagentCompletion.decode(f.children[child]?.subagentResultJson))
        val queued = checkNotNull(f.registry.get("parent")).queuedSends.value.single()
        assertTrue(queued.text.endsWith("Partial progress\nError: Socket is closed\nError code: connection_closed"))
        assertEquals("Socket is closed", queued.source?.subagentResult?.errorText)
        assertEquals(mapOf(child to expected.id), queued.source?.subagentResults)
        assertEquals(listOf(child to expected), f.manager.wait(listOf(child), 1L, f.context))
        f.summaries.value = listOf(SubagentSummary(child, "Worker", null, "parent", "failed"))
        val listed = f.manager.list(f.context).single()
        assertEquals("Socket is closed", listed.errorText)
        assertEquals("connection_closed", listed.errorCode)
        coVerify(exactly = 0) { f.dao.getLastMessageForConversation(any()) }
    }

    @Test
    fun exactFailureWinsOverAReplacementStreamingOverlay() = runTest {
        val f = SubagentManagerTest.Fixture(backgroundScope)
        coEvery { f.engine.runOnceWithAutomationGuardsHeld(any(), any(), any(), any(), any(), any(), any(), any(), any()) } coAnswers {
            val child = firstArg<String>()
            f.terminalMessages["failed-answer"] = MessageEntity(
                id = "failed-answer", conversationId = child, text = "Partial progress",
                participant = Participant.MODEL, status = MessageStatus.ERROR, timestamp = 2L,
                runId = "failed-run", runSequence = 1L,
                toolCallJson = Json.encodeToString(listOf(
                    MessageSegment("answer", "Partial progress"), MessageSegment("error", "Socket is closed"),
                )),
            )
            f.answer(child, "replacement", "Replacement output")
            val state = f.registry.getOrCreate(child)
            val token = checkNotNull(state.tryAcquireForReplacement())
            state.streamUpdate(token, ChatMessage(id = "replacement", text = "Replacement output",
                participant = Participant.MODEL, status = MessageStatus.SUCCESS))
            TaskExecutionEngine.Result.Failure("Partial progress", "failed-answer")
        }
        val child = f.spawn()
        runCurrent()
        val result = checkNotNull(SubagentCompletion.decode(f.children[child]?.subagentResultJson))
        assertEquals("failed-answer", result.id)
        assertEquals("failed", result.status)
        assertEquals("Partial progress", result.text)
        assertEquals("Socket is closed", result.errorText)
        assertEquals(1, checkNotNull(f.registry.get("parent")).queuedSends.value.size)
        f.registry.get(child)?.dispose()
    }
}
