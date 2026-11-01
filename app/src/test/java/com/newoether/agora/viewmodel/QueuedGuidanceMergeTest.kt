package com.newoether.agora.viewmodel

import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.MessageSource
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class QueuedGuidanceMergeTest {
    @Test
    fun claimPreservesSeparateFifoMessagesSnapshotsAndLocalAttachmentMetadata() {
        val olderSnapshot = testGenerationAdmissionSnapshot(
            conversationId = "conversation", runId = "older-run", selectedModelId = "older-model",
        )
        val latestSnapshot = testGenerationAdmissionSnapshot(
            conversationId = "conversation", runId = "latest-run", selectedModelId = "latest-model",
        )
        val original = listOf(
            queued("one", "first").copy(
                modelId = "older-model", generationSnapshot = olderSnapshot,
                preparedImages = listOf("one.png"), preparedAttachmentMetaJson = imageMeta("one.png"),
            ),
            queued("two", "second").copy(
                modelId = "latest-model", generationSnapshot = latestSnapshot,
                preparedImages = listOf("two.png"), preparedAttachmentMetaJson = imageMeta("two.png"),
            ),
        )
        val store = GuidanceLeaseStore({}) { "lease" }
        original.forEach(store::enqueue)

        val lease = checkNotNull(store.claim())

        assertEquals(original, lease.batch)
        assertEquals(listOf("one", "two"), lease.batch.map(QueuedSend::id))
        assertEquals(listOf("first", "second"), lease.batch.map(QueuedSend::text))
        assertEquals(listOf(olderSnapshot, latestSnapshot), lease.batch.map { it.generationSnapshot })
        assertEquals(listOf(listOf("one.png"), listOf("two.png")), lease.batch.map { it.preparedImages })
        val items = lease.batch.map {
            Json.decodeFromString<AttachmentMeta>(checkNotNull(it.preparedAttachmentMetaJson)).items.single()
        }
        assertEquals(listOf("one.png", "two.png"), items.map(AttachmentItem::fileName))
        assertEquals(listOf(0, 0), items.map(AttachmentItem::imageIndex))
        assertTrue(store.queuedSends.value.isEmpty())
        assertTrue(store.settle(lease.id, durable = true))
    }

    @Test
    fun claimClearsTheWholePendingBatchOnceAndDurableSettlementKeepsNewArrivals() {
        val store = GuidanceLeaseStore({}) { "lease" }
        val original = listOf(queued("one", "first"), queued("two", "second"))
        original.forEach(store::enqueue)

        val lease = checkNotNull(store.claim())

        assertEquals(original, lease.batch)
        assertTrue(store.queuedSends.value.isEmpty())
        assertNull(store.claim())
        val later = queued("three", "later")
        store.enqueue(later)
        assertTrue(store.settle(lease.id, durable = true))
        assertFalse(store.settle(lease.id, durable = false))
        assertEquals(listOf(later), store.queuedSends.value)
        assertTrue(store.settle(checkNotNull(store.claim()).id, durable = true))
    }

    @Test
    fun failedClaimReturnsExactOriginalMessagesAheadOfNewArrivalsWithoutParsingMetadata() {
        val store = GuidanceLeaseStore({}) { "lease" }
        val first = queued("one", "first").copy(preparedAttachmentMetaJson = "{")
        val second = queued("two", "second")
        store.enqueue(first)
        store.enqueue(second)
        val lease = checkNotNull(store.claim())
        val later = queued("three", "later")
        store.enqueue(later)

        assertEquals(listOf(first, second), lease.batch)
        assertTrue(store.settle(lease.id, durable = false))
        assertFalse(store.settle(lease.id, durable = false))
        assertEquals(listOf(first, second, later), store.queuedSends.value)
        assertTrue(store.settle(checkNotNull(store.claim()).id, durable = true))
    }

    @Test
    fun adjacentTypedAnswersAndSubagentEntriesKeepIndividualTextSourcesAndReceipts() {
        fun answer(id: String, question: String, reply: String?) = queued(
            id, "$question\n${reply ?: MessageSource.NO_ANSWER}",
        ).copy(source = MessageSource.askUser(listOf(MessageSource.AskUserItem(question, reply))))
        fun completion(id: String) = queued(id, "Result $id").copy(
            source = MessageSource(MessageSource.Kind.SUBAGENT, subagentResults = mapOf(id to "result-$id")),
        )
        val original = listOf(
            queued("t1", "typed one"), queued("t2", "typed two"),
            answer("a1", "Port?", "8080"), answer("a2", "Restart?", null),
            completion("s1"), completion("s2"), queued("t3", "typed three"),
        )
        val store = GuidanceLeaseStore({}) { "lease" }
        original.forEach(store::enqueue)

        val lease = checkNotNull(store.claim())

        assertEquals(original, lease.batch)
        assertEquals(listOf("t1", "t2", "a1", "a2", "s1", "s2", "t3"), lease.batch.map { it.id })
        assertEquals(
            listOf("typed one", "typed two", "Port?\n8080", "Restart?\n${MessageSource.NO_ANSWER}",
                "Result s1", "Result s2", "typed three"),
            lease.batch.map { it.text },
        )
        assertEquals(
            listOf(null, null, MessageSource.Kind.ASK_USER, MessageSource.Kind.ASK_USER,
                MessageSource.Kind.SUBAGENT, MessageSource.Kind.SUBAGENT, null),
            lease.batch.map { it.source?.kind },
        )
        assertEquals(listOf(MessageSource.AskUserItem("Port?", "8080")), lease.batch[2].source?.askUser)
        assertEquals(listOf(MessageSource.AskUserItem("Restart?", null)), lease.batch[3].source?.askUser)
        assertEquals(mapOf("s1" to "result-s1"), lease.batch[4].source?.subagentResults)
        assertEquals(mapOf("s2" to "result-s2"), lease.batch[5].source?.subagentResults)
        assertTrue(store.settle(lease.id, durable = true))
    }

    @Test
    fun automationRequestKindsMapToTheirSource() {
        assertEquals(MessageSource.TASK, MessageSource.forAutomationRequestKind("task"))
        assertEquals(MessageSource.LOOP, MessageSource.forAutomationRequestKind("loop"))
        assertEquals(MessageSource.Kind.SUBAGENT, MessageSource.forAutomationRequestKind("subagent")?.kind)
        assertNull(MessageSource.forAutomationRequestKind("chat"))
        assertNull(MessageSource.forAutomationRequestKind("queued_guidance"))
    }

    private fun imageMeta(fileName: String) = Json.encodeToString(
        AttachmentMeta(listOf(AttachmentItem(type = "image", fileName = fileName, imageIndex = 0))),
    )

    private fun queued(id: String, text: String) = QueuedSend(
        id = id, text = text, modelId = "model", attachments = emptyList(), runId = "old-run",
    )
}
