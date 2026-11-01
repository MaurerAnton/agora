package com.newoether.agora.data

import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.Participant
import com.newoether.agora.model.ToolCallData
import com.newoether.agora.ui.chat.message.ToolPresentationResolver
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotSame
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Test

class MessageDisplayFastPathTest {
    private val id = "custom-provider-00000000-0000-4000-8000-000000000001"
    private val providers = listOf(CustomProviderConfig(name = "Relay X", id = id))
    private val plain = ChatMessage(
        text = "answer",
        participant = Participant.MODEL,
        thoughts = "reason",
        toolCall = ToolCallData("shell", "{}", "{}"),
        segments = listOf(MessageSegment("tool", toolName = "shell", toolArgs = "{}", toolResult = "{}")),
        attachmentMeta = AttachmentMeta(listOf(AttachmentItem(type = "file", fileName = "a.txt"))),
    )

    @Test
    fun messageWithoutProviderIdsIsReturnedAsIs() {
        assertSame(plain, plain.forDisplay(providers))
    }

    @Test
    fun anyDisplayedFieldHoldingAnIdStillGetsItsDisplayName() {
        val tool = plain.segments!!.single()
        val variants = listOf(
            plain.copy(text = "via $id"),
            plain.copy(thoughts = "via $id"),
            plain.copy(thoughtTitle = "via $id"),
            plain.copy(retryText = "via $id"),
            plain.copy(toolCall = plain.toolCall!!.copy(arguments = "via $id")),
            plain.copy(toolCall = plain.toolCall!!.copy(structuredResult = "via $id")),
            plain.copy(segments = listOf(tool.copy(toolArgs = "via $id"))),
            plain.copy(segments = listOf(tool.copy(toolResult = "via $id"))),
            plain.copy(segments = listOf(tool.copy(toolStructuredResult = "via $id"))),
            plain.copy(segments = listOf(tool.copy(toolProgress = "via $id"))),
            plain.copy(attachmentMeta = AttachmentMeta(listOf(AttachmentItem(type = "file", textContent = "via $id")))),
        )
        variants.forEach { message ->
            val displayed = message.forDisplay(providers)
            assertNotSame(message, displayed)
            assertTrue(displayed.toString(), "Relay X" in displayed.toString() && id !in displayed.toString())
        }
    }

    @Test
    fun finishedToolSegmentResolvesToTheSamePresentation() {
        val segment = MessageSegment("tool", toolName = "file_read", toolArgs = """{"path":"a"}""",
            toolResult = """{"content":"x"}""")
        assertSame(ToolPresentationResolver.resolve(segment), ToolPresentationResolver.resolve(segment.copy()))
        val live = segment.copy(toolResult = null)
        assertEquals(ToolPresentationResolver.resolve(live), ToolPresentationResolver.resolve(live))
    }
}
