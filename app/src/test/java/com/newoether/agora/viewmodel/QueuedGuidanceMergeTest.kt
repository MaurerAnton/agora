package com.newoether.agora.viewmodel

import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import kotlinx.serialization.SerializationException
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test

class QueuedGuidanceMergeTest {
    @Test
    fun drainMergesFifoTextAndAttachmentOwnershipIntoOneBubble() {
        val firstMeta = Json.encodeToString(
            AttachmentMeta(
                listOf(AttachmentItem(type = "image", fileName = "one.png", imageIndex = 0)),
            )
        )
        val secondMeta = Json.encodeToString(
            AttachmentMeta(
                listOf(AttachmentItem(type = "image", fileName = "two.png", imageIndex = 0)),
            )
        )
        val olderSnapshot = testGenerationAdmissionSnapshot(
            conversationId = "conversation",
            runId = "older-run",
            selectedModelId = "older-model",
        )
        val latestSnapshot = testGenerationAdmissionSnapshot(
            conversationId = "conversation",
            runId = "latest-run",
            selectedModelId = "latest-model",
        )
        val merged = mergeQueuedGuidance(
            listOf(
                queued("one", "first").copy(
                    modelId = "older-model",
                    generationSnapshot = olderSnapshot,
                    preparedImages = listOf("one.png"),
                    preparedAttachmentMetaJson = firstMeta,
                ),
                queued("two", "second").copy(
                    modelId = "latest-model",
                    generationSnapshot = latestSnapshot,
                    preparedImages = listOf("two.png"),
                    preparedAttachmentMetaJson = secondMeta,
                ),
            )
        )

        assertEquals("one", merged.id)
        assertEquals("first\n\nsecond", merged.text)
        assertEquals("latest-model", merged.modelId)
        assertEquals(latestSnapshot, merged.generationSnapshot)
        assertEquals(listOf("one.png", "two.png"), merged.preparedImages)
        val items = Json.decodeFromString<AttachmentMeta>(
            checkNotNull(merged.preparedAttachmentMetaJson),
        ).items
        assertEquals(listOf("one.png", "two.png"), items.map(AttachmentItem::fileName))
        assertEquals(listOf(0, 1), items.map(AttachmentItem::imageIndex))
    }

    @Test
    fun mergeFailureRestoresTheExactOriginalLeaseBatch() {
        val store = GuidanceLeaseStore { "lease" }
        val first = queued("one", "first").copy(preparedAttachmentMetaJson = "{")
        val second = queued("two", "second")
        store.enqueue(first)
        store.enqueue(second)
        val lease = checkNotNull(store.claim())

        assertThrows(SerializationException::class.java) {
            mergeQueuedGuidance(lease.batch)
        }
        assertTrue(store.settle(lease.id, durable = false))
        assertEquals(listOf(first, second), store.queuedSends.value)
    }

    private fun queued(id: String, text: String) = QueuedSend(
        id = id,
        text = text,
        modelId = "model",
        attachments = emptyList(),
        runId = "old-run",
    )
}
