package com.newoether.agora.data.repository

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.automation.ConversationExecutionCoordinator
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import com.newoether.agora.model.SubagentCompletion
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class ConversationFamilyRecoveryRoomTest {
    private lateinit var database: ChatDatabase
    private lateinit var dao: ChatDao
    private lateinit var repository: ConversationRepository

    @Before fun setUp() {
        database = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext<Context>(), ChatDatabase::class.java,
        ).allowMainThreadQueries().build()
        dao = database.chatDao()
        repository = ConversationRepository(dao, database, scheduleMaintenance = {})
    }

    @After fun tearDown() = database.close()

    @Test fun familyRecoveryChangesOnlyStatusAndKeepsConsumedOutboxAndUnrelatedOwner() = runTest {
        dao.upsertConversation(ChatEntity("parent", "Parent"))
        val old = SubagentCompletion("synthetic-old", "completed", "Old delivered result")
        val child = ChatEntity("child", "Child", parentConversationId = "parent",
            subagentResultId = old.id, subagentResultJson = SubagentCompletion.encode(old),
            subagentResultStatus = old.status, subagentDeliveredResultId = old.id,
            draftText = "keep draft", lastUpdated = 10L)
        dao.upsertConversation(child)
        dao.insertRun(activeRun("child", "interrupted", 20L))
        dao.upsertMessage(model("child", "interrupted", "partial", MessageStatus.SENDING))
        val unrelated = ChatEntity("unrelated", "Unrelated", draftText = "keep unrelated")
        dao.upsertConversation(unrelated)
        dao.insertRun(activeRun("unrelated", "unrelated-run", 20L))

        repository.recoverOpenedConversationFamily("parent", ConversationExecutionCoordinator())

        val after = checkNotNull(dao.getConversation("child"))
        assertEquals("failed", after.subagentResultStatus)
        assertEquals(child.copy(subagentResultStatus = "failed", dataChangedAt = after.dataChangedAt), after)
        assertEquals(RunStatus.STOPPED, dao.getRun("interrupted")?.status)
        assertEquals(RunEndReason.PROCESS_RECOVERED, dao.getRun("interrupted")?.endReason)
        assertEquals(MessageStatus.STOPPED, dao.getMessage("partial")?.status)
        assertEquals("failed", dao.observeSubagents("parent").first().single().status)
        assertTrue(dao.pendingSubagentResults("parent").isEmpty())
        assertEquals(unrelated, dao.getConversation("unrelated"))
        assertEquals(RunStatus.ACTIVE, dao.getRun("unrelated-run")?.status)
    }

    @Test fun laterExactSuccessClearsMessageBackedFailureWithoutRepublishingCompletion() = runTest {
        dao.upsertConversation(ChatEntity("parent", "Parent"))
        dao.upsertConversation(ChatEntity("child", "Child", parentConversationId = "parent"))
        dao.insertRun(terminalRun("child", "old-run", 1L, RunStatus.FAILED, RunEndReason.PROVIDER_ERROR))
        dao.upsertMessage(model("child", "old-run", "old-message", MessageStatus.ERROR))
        val old = SubagentCompletion("old-message", "failed", "Old error", "old-message")
        dao.setSubagentResult("child", "parent", SubagentCompletion.encode(old), old.id, old.status, 10L)
        dao.acknowledgeSubagentResult("child", "parent", old.id, 11L)
        val before = checkNotNull(dao.getConversation("child"))
        dao.insertRun(terminalRun("child", "fresh-run", 20L, RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED))
        dao.upsertMessage(model("child", "fresh-run", "fresh-message", MessageStatus.SUCCESS))

        repository.recoverOpenedConversationFamily("parent", ConversationExecutionCoordinator())

        val after = checkNotNull(dao.getConversation("child"))
        assertEquals("completed", after.subagentResultStatus)
        assertEquals(before.copy(subagentResultStatus = "completed", dataChangedAt = after.dataChangedAt), after)
        assertTrue(dao.pendingSubagentResults("parent").isEmpty())
        assertEquals("completed", dao.observeSubagents("parent").first().single().status)
    }

    @Test fun syntheticAdmissionFailureWithoutOrderingRemainsFailed() = runTest {
        dao.upsertConversation(ChatEntity("parent", "Parent"))
        val old = SubagentCompletion("admission-failed", "failed", "Synthetic failure")
        dao.upsertConversation(ChatEntity("child", "Child", parentConversationId = "parent",
            subagentResultId = old.id, subagentResultJson = SubagentCompletion.encode(old),
            subagentResultStatus = old.status, subagentDeliveredResultId = old.id))
        dao.insertRun(terminalRun("child", "old-success", 1L, RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED))
        dao.upsertMessage(model("child", "old-success", "success-message", MessageStatus.SUCCESS))
        val before = dao.getConversation("child")

        repository.recoverOpenedConversationFamily("parent", ConversationExecutionCoordinator())

        assertEquals(before, dao.getConversation("child"))
        assertEquals("failed", dao.observeSubagents("parent").first().single().status)
        assertTrue(dao.pendingSubagentResults("parent").isEmpty())
    }

    private fun activeRun(owner: String, id: String, startedAt: Long) = RunEntity(
        id, owner, null, RunStatus.ACTIVE, 1, startedAt, startedAt,
    )

    private fun terminalRun(
        owner: String, id: String, startedAt: Long, status: RunStatus, reason: RunEndReason,
    ) = RunEntity(id, owner, null, status, null, startedAt, startedAt + 1,
        endedAt = startedAt + 1, endReason = reason)

    private fun model(owner: String, runId: String, id: String, status: MessageStatus) = MessageEntity(
        id = id, conversationId = owner, text = "Payload is not a success classifier",
        participant = Participant.MODEL, status = status, timestamp = 1L,
        runId = runId, runSequence = 0L,
    )
}
