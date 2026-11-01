package com.newoether.agora.data.repository

import android.content.Context
import androidx.room.Room
import androidx.test.core.app.ApplicationProvider
import com.newoether.agora.data.local.*
import com.newoether.agora.model.*
import com.newoether.agora.util.Constants
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner

@RunWith(RobolectricTestRunner::class)
class ConversationSubagentsTest {
    private lateinit var database: ChatDatabase
    private lateinit var dao: ChatDao
    private lateinit var repository: ConversationRepository
    private val identity = RunEffectIdentity("parent", 1L, "r-parent", 0, "effect")

    @Before fun setUp() {
        database = Room.inMemoryDatabaseBuilder(
            ApplicationProvider.getApplicationContext<Context>(), ChatDatabase::class.java,
        ).allowMainThreadQueries().build()
        dao = database.chatDao()
        repository = ConversationRepository(dao, database, scheduleMaintenance = {})
    }
    @After fun tearDown() = database.close()

    private fun run(owner: String, id: String = "r-$owner") = RunEntity(
        id = id, conversationId = owner, parentRunId = null,
        status = RunStatus.ACTIVE, activeSlot = 1, currentPass = 0,
        startedAt = 1L, lastCheckpointAt = 1L,
    )
    private suspend fun seed() {
        dao.upsertConversation(ChatEntity("parent", "Parent", draftText = "keep"))
        dao.insertRun(run("parent"))
        dao.upsertConversation(ChatEntity("other", "Other"))
    }
    private suspend fun child(id: String = "child") {
        repository.createSubagent(identity, ChatEntity(id, id, parentConversationId = "parent"), "{}")
        repository.recordSubagentCompletion(id, "parent", SubagentCompletion("result-$id", "completed", "Answer"))
    }
    private fun input(owner: String, runId: String, source: MessageSource? = null) = listOf(
        MessageEntity(id = "u-$runId", conversationId = owner, text = "Input", participant = Participant.USER,
            timestamp = 1L, runId = runId, runSequence = 0L, sourceJson = MessageSource.encode(source)),
        MessageEntity(id = "m-$runId", conversationId = owner, parentId = "u-$runId", text = "",
            participant = Participant.MODEL, status = MessageStatus.SENDING,
            timestamp = 2L, runId = runId, runSequence = 1L),
    )
    private suspend fun admit(owner: String, id: String, source: MessageSource? = null) =
        repository.createRunWithMessages(run(owner, id), input(owner, id, source), emptyMap(), null,
            at = 20L, touchConversationOnAdmission = false)

    @Test fun childCreationIsIsolatedAndStalePassOrNestedParentCannotCreate() = runTest {
        seed()
        child()
        assertTrue(dao.getMessageContextTopology("child").isEmpty())
        assertEquals("{}", dao.getConversationSettingsTransfer("child")?.settingsJson)
        assertTrue(runCatching {
            repository.createSubagent(identity.copy(pass = 1), ChatEntity("bad", "Bad", parentConversationId = "parent"), null)
        }.isFailure)
        dao.insertRun(run("child"))
        assertTrue(runCatching {
            repository.createSubagent(identity.copy(conversationId = "child", runId = "r-child"),
                ChatEntity("nested", "Nested", parentConversationId = "child"), null)
        }.isFailure)
        assertNull(dao.getConversation("bad"))
        assertNull(dao.getConversation("nested"))
        assertEquals("keep", dao.getConversation("parent")?.draftText)
    }

    @Test fun countLimitIncludesFinishedChildrenAndNeverWritesRejectedChildOrTransfer() = runTest {
        seed()
        child("finished")
        assertEquals(1, dao.countSubagents("parent"))
        val failure = runCatching {
            repository.createSubagent(identity,
                ChatEntity("rejected", "Rejected", parentConversationId = "parent"), "{}", maxSubagents = 1)
        }.exceptionOrNull()
        assertTrue(failure is IllegalStateException)
        assertNull(dao.getConversation("rejected"))
        assertNull(dao.getConversationSettingsTransfer("rejected"))
        assertNotNull(dao.getConversation("finished"))
        dao.insertRun(run("other"))
        repository.createSubagent(identity.copy(conversationId = "other", runId = "r-other"),
            ChatEntity("other-child", "Other child", parentConversationId = "other"), null, maxSubagents = 1)
        assertEquals(1, dao.countSubagents("other"))
        assertEquals("keep", dao.getConversation("parent")?.draftText)
    }

    @Test fun concurrentCreationOfLastAvailableSlotAdmitsExactlyOneChildAndTransfer() = runTest {
        seed()
        child("existing")
        val candidates = (0 until 8).map { "candidate-$it" }
        val outcomes = candidates.map { id ->
            async(Dispatchers.IO) {
                runCatching {
                    repository.createSubagent(identity,
                        ChatEntity(id, id, parentConversationId = "parent"), "{}", maxSubagents = 2)
                    id
                }
            }
        }.awaitAll()
        assertEquals(1, outcomes.count { it.isSuccess })
        assertEquals(2, dao.countSubagents("parent"))
        val accepted = outcomes.single { it.isSuccess }.getOrThrow()
        candidates.forEach { id ->
            if (id == accepted) {
                assertNotNull(dao.getConversation(id))
                assertEquals("{}", dao.getConversationSettingsTransfer(id)?.settingsJson)
            } else {
                assertNull(dao.getConversation(id))
                assertNull(dao.getConversationSettingsTransfer(id))
            }
        }
        assertTrue(dao.getMessageContextTopology("other").isEmpty())
    }

    @Test fun receiptAndAcceptedInputCommitTogetherAndFailuresRollbackBoth() = runTest {
        seed()
        child()
        val source = MessageSource(MessageSource.Kind.SUBAGENT, subagentResults = mapOf("child" to "result-child"))
        // Existing live parent Run rejects admission after the receipt update; Room rolls it back.
        assertTrue(runCatching { admit("parent", "blocked", source) }.isFailure)
        assertNull(dao.getConversation("child")?.subagentDeliveredResultId)
        assertNull(dao.getRun("blocked"))
        assertNull(dao.getMessage("u-blocked"))
        dao.terminalizeLiveRun("r-parent", RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED, 10L)
        admit("parent", "accepted", source)
        assertEquals("result-child", dao.getConversation("child")?.subagentDeliveredResultId)
        assertEquals(source, MessageSource.decode(dao.getMessage("u-accepted")?.sourceJson))
        assertTrue(repository.pendingSubagentResults("parent").isEmpty())
        dao.terminalizeLiveRun("accepted", RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED, 30L)
        assertTrue(runCatching { admit("parent", "duplicate", source) }.isFailure)
        assertNull(dao.getRun("duplicate"))
    }

    @Test fun staleBatchOfWaitReceiptsRollsBackPreviouslyAcknowledgedChildren() = runTest {
        seed()
        child("a")
        child("b")
        assertTrue(runCatching {
            repository.consumeSubagentCompletions(identity, linkedMapOf("a" to "result-a", "b" to "stale"))
        }.isFailure)
        assertNull(dao.getConversation("a")?.subagentDeliveredResultId)
        repository.consumeSubagentCompletions(identity, mapOf("a" to "result-a", "b" to "result-b"))
        assertTrue(repository.pendingSubagentResults("parent").isEmpty())
        assertTrue(runCatching { repository.consumeSubagentCompletions(identity.copy(pass = 1), emptyMap()) }.isFailure)
    }

    @Test fun queuedFollowupHandoffCannotPublishEmptyOrPartialCompletion() = runTest {
        seed()
        child()
        val previous = dao.getConversation("child")
        listOf("", "Partial progress").forEachIndexed { index, text ->
            val runId = "handoff-$index"
            admit("child", runId, MessageSource(MessageSource.Kind.SUBAGENT))
            val message = ChatMessage(id = "m-$runId", text = text, participant = Participant.MODEL,
                status = MessageStatus.SUCCESS, runId = runId, runSequence = 1L)
            assertTrue(repository.finishGeneration(message, "child", runId, RunStatus.COMPLETED,
                RunEndReason.CONTINUATION_PENDING))
            assertEquals(RunEndReason.CONTINUATION_PENDING, dao.getRun(runId)?.endReason)
            assertNull(repository.subagentTerminalCompletion("child", message.id))
            val current = dao.getConversation("child")
            assertEquals(previous?.subagentResultId, current?.subagentResultId)
            assertEquals(previous?.subagentResultJson, current?.subagentResultJson)
            assertEquals(previous?.subagentDeliveredResultId, current?.subagentDeliveredResultId)
            assertNull(dao.getLiveRun("child"))
        }
        admit("child", "final", MessageSource(MessageSource.Kind.SUBAGENT))
        assertTrue(repository.finishGeneration(ChatMessage(id = "m-final", text = "Final answer",
            participant = Participant.MODEL, status = MessageStatus.SUCCESS, runId = "final", runSequence = 1L),
            "child", "final", RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED))
        assertEquals(SubagentCompletion("m-final", "completed", "Final answer", "m-final"),
            SubagentCompletion.decode(dao.getConversation("child")?.subagentResultJson))
        assertEquals(listOf("m-final"), repository.pendingSubagentResults("parent").map { it.resultId })
        assertEquals("keep", dao.getConversation("parent")?.draftText)
        assertTrue(dao.getMessageContextTopology("other").isEmpty())
    }

    @Test fun terminalOutboxFollowsOnlyCommittedRunAndStopPersistsStoppedResult() = runTest {
        seed()
        child()
        admit("child", "child-run")
        val message = ChatMessage(id = "m-child-run", text = "Finished", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = "child-run", runSequence = 1L)
        assertFalse(repository.finishGeneration(message, "child", "stale", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        assertEquals("result-child", dao.getConversation("child")?.subagentResultId)
        assertTrue(repository.finishStoppedGeneration(listOf(message), "child-run"))
        val completion = SubagentCompletion.decode(dao.getConversation("child")?.subagentResultJson)
        assertEquals("stopped", completion?.status)
        assertEquals("m-child-run", completion?.id)
        assertNull(dao.getLiveRun("child"))
        val terminal = dao.getConversation("child")
        assertFalse(repository.finishGeneration(message, "child", "child-run", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        assertEquals(terminal, dao.getConversation("child"))
    }

    @Test fun terminalFailureKeepsAnswerAndPersistedErrorAsSeparateCompletionFields() = runTest {
        seed()
        child()
        admit("child", "failed-run")
        val message = ChatMessage(id = "m-failed-run", text = "Partial answer", participant = Participant.MODEL,
            status = MessageStatus.ERROR, runId = "failed-run", runSequence = 1L,
            segments = listOf(MessageSegment("answer", "Partial answer"),
                MessageSegment("error", "Provider connection closed", errorCode = "connection_closed")))
        assertTrue(repository.finishGeneration(message, "child", "failed-run", RunStatus.FAILED,
            RunEndReason.PROVIDER_ERROR))
        val result = checkNotNull(SubagentCompletion.decode(dao.getConversation("child")?.subagentResultJson))
        assertEquals("failed", result.status)
        assertEquals("Partial answer", result.text)
        assertEquals("Provider connection closed", result.errorText)
        assertEquals("connection_closed", result.errorCode)
        assertEquals(message.id, result.modelMessageId)
        assertEquals(result, repository.subagentTerminalCompletion("child", message.id))
        assertEquals(result, repository.subagentFailureDetail("parent", "child"))
        assertNull(repository.subagentFailureDetail("other", "child"))
        assertEquals(result, SubagentCompletion.decode(repository.pendingSubagentResults("parent").single().resultJson))
    }

    @Test fun failedReceiptUsesTheChildsGrayErrorTextIncludingTheMissingDetailFallback() = runTest {
        seed()
        child()
        val context = ApplicationProvider.getApplicationContext<Context>()
        repository = ConversationRepository(dao, database, scheduleMaintenance = {}, generationErrorFormatter = { raw ->
            com.newoether.agora.viewmodel.normalizePersistedGenerationErrorText(
                context, raw.ifBlank { context.getString(com.newoether.agora.R.string.failed_to_generate) },
            )
        })
        val cases = listOf(
            "explicit" to listOf(MessageSegment("answer", "Partial answer"),
                MessageSegment("error", "connection closed", errorCode = "connection_closed")),
            "missing" to listOf(MessageSegment("answer", "Partial answer")),
            "blank" to listOf(MessageSegment("answer", "Partial answer"), MessageSegment("error", " ")),
        )
        for ((id, segments) in cases) {
            admit("child", id)
            val message = ChatMessage(id = "m-$id", text = "Partial answer", participant = Participant.MODEL,
                status = MessageStatus.ERROR, runId = id, runSequence = 1L, segments = segments)
            assertTrue(repository.finishGeneration(message, "child", id, RunStatus.FAILED, RunEndReason.PROVIDER_ERROR))
            val result = checkNotNull(SubagentCompletion.decode(dao.getConversation("child")?.subagentResultJson))
            val visible = checkNotNull(com.newoether.agora.ui.chat.message.assistantErrorContent(
                message, segments, context.getString(com.newoether.agora.R.string.failed_to_generate),
            ))
            assertEquals(com.newoether.agora.viewmodel.normalizePersistedGenerationErrorText(context, visible.errorText),
                result.errorText)
            assertEquals("Partial answer", result.text)
            assertNotEquals(result.text, result.errorText)
            assertEquals(segments.lastOrNull { it.type == "error" && it.content.isNotBlank() }?.errorCode, result.errorCode)
            assertEquals(result, repository.subagentFailureDetail("parent", "child"))
            assertEquals(result, SubagentCompletion.decode(repository.pendingSubagentResults("parent").single().resultJson))
        }
    }

    @Test fun exactRecoveredTerminalReplacesFailedCacheDespiteNewerOrTiedUnrelatedRows() = runTest {
        seed()
        child()
        val old = SubagentCompletion("old-failed", "failed", "Old failure")
        repository.recordSubagentCompletion("child", "parent", old)
        repository.consumeSubagentCompletions(identity, mapOf("child" to old.id))
        admit("child", "recovered")
        val decoys = listOf(
            MessageEntity(id = "newer-user", conversationId = "child", text = "Continue",
                participant = Participant.USER, timestamp = 1000L, runId = "recovered", runSequence = 2L),
            MessageEntity(id = "tied-tool", conversationId = "child", text = "",
                participant = Participant.USER, timestamp = 2L, runId = "recovered", runSequence = 3L,
                toolCallJson = "[]"),
            MessageEntity(id = Constants.COMPACT_MSG_PREFIX + "decoy", conversationId = "child", text = "Summary",
                participant = Participant.MODEL, status = MessageStatus.SUCCESS, timestamp = 1001L,
                runId = "recovered", runSequence = 4L),
            MessageEntity(id = "branch-decoy", conversationId = "child", text = "Unselected old error",
                participant = Participant.MODEL, status = MessageStatus.ERROR, timestamp = 1002L,
                runId = "recovered", runSequence = 5L),
        )
        decoys.forEach { dao.upsertMessage(it) }
        val otherBefore = dao.getConversation("other")
        val message = ChatMessage(id = "m-recovered", text = "Recovered answer", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = "recovered", runSequence = 1L)
        assertEquals("branch-decoy", dao.getLastMessageForConversation("child")?.id)
        assertTrue(repository.finishGeneration(message, "child", "recovered", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        val expected = SubagentCompletion(message.id, "completed", message.text, message.id)
        val after = checkNotNull(dao.getConversation("child"))
        assertEquals(expected, SubagentCompletion.decode(after.subagentResultJson))
        assertEquals(old.id, after.subagentDeliveredResultId)
        assertEquals("completed", repository.observeSubagents("parent").first().single().status)
        assertEquals(expected.id, repository.pendingSubagentResults("parent").single().resultId)
        decoys.forEach { assertEquals(it, dao.getMessage(it.id)) }
        assertEquals(otherBefore, dao.getConversation("other"))
        repository.consumeSubagentCompletions(identity, mapOf("child" to expected.id))
        val consumed = dao.getConversation("child")
        assertEquals(expected, repository.subagentTerminalCompletion("child", expected.id))
        assertEquals(consumed, dao.getConversation("child"))
        assertTrue(repository.pendingSubagentResults("parent").isEmpty())
    }

    @Test fun terminalProjectionDistinguishesProcessRecoveryFromExplicitStopAndRejectsUnsafeIdentity() = runTest {
        seed()
        child()
        admit("child", "interrupted")
        val message = ChatMessage(id = "m-interrupted", text = "Success words do not classify status",
            participant = Participant.MODEL, status = MessageStatus.STOPPED,
            runId = "interrupted", runSequence = 1L)
        assertTrue(repository.finishStoppedGeneration(listOf(message), "interrupted"))
        assertEquals("stopped", repository.subagentTerminalCompletion("child", message.id)?.status)
        val stoppedRun = checkNotNull(dao.getRun("interrupted"))
        dao.upsertRun(stoppedRun.copy(endReason = RunEndReason.PROCESS_RECOVERED))
        val before = dao.getConversation("child")
        assertEquals("failed", repository.subagentTerminalCompletion("child", message.id)?.status)
        assertEquals(before, dao.getConversation("child"))
        assertEquals(RunStatus.STOPPED, dao.getRun("interrupted")?.status)
        assertEquals(MessageStatus.STOPPED, dao.getMessage(message.id)?.status)
        listOf(null, "missing", "u-interrupted").forEach { id -> repository.persistSubagentTerminal("child", 30L, id) }
        assertNull(repository.subagentTerminalCompletion("other", message.id))
        assertEquals(before, dao.getConversation("child"))
        dao.upsertRun(stoppedRun.copy(legacyAmbiguous = true))
        assertNull(repository.subagentTerminalCompletion("child", message.id))
        assertEquals(before, dao.getConversation("child"))
    }

    @Test fun taskRunHandoffsDoNotPublishUntilWorkerCompletes() = runTest {
        seed()
        repository.createSubagent(identity, ChatEntity("child", "Worker", parentConversationId = "parent"), null)
        val worker = Job()
        repository.subagentJobs.value = mapOf("child" to worker)
        suspend fun status() = repository.observeSubagents("parent").first().single().status
        listOf("compact-pre-send", "ordinary", "compact-tool-round", "continuation").forEachIndexed { index, id ->
            val messageId = if (id.startsWith("compact")) Constants.COMPACT_MSG_PREFIX + id else "m-$id"
            val messages = input("child", id).map {
                it.copy(id = if (it.participant == Participant.MODEL) messageId else it.id,
                    timestamp = it.timestamp + 10L * index)
            }
            repository.createRunWithMessages(run("child", id), messages, emptyMap(), null,
                at = 20L, touchConversationOnAdmission = false)
            assertEquals("running", status())
            val message = ChatMessage(id = messageId, text = "Finished $id", participant = Participant.MODEL,
                status = MessageStatus.SUCCESS, runId = id, runSequence = 1L)
            assertTrue(repository.finishGeneration(message, "child", id, RunStatus.COMPLETED,
                RunEndReason.MODEL_COMPLETED))
            assertEquals(RunStatus.COMPLETED, dao.getRun(id)?.status)
            assertNull(dao.getLiveRun("child"))
            assertEquals("running", status())
            assertNull(dao.getConversation("child")?.subagentResultId)
            assertTrue(repository.pendingSubagentResults("parent").isEmpty())
        }
        val completion = SubagentCompletion("m-continuation", "completed", "Finished continuation", "m-continuation")
        assertTrue(repository.recordSubagentCompletion("child", "parent", completion))
        worker.complete()
        repository.subagentJobs.value = emptyMap()
        assertEquals("completed", status())
        assertEquals(completion, SubagentCompletion.decode(repository.pendingSubagentResults("parent").single().resultJson))
        assertEquals("keep", dao.getConversation("parent")?.draftText)
        assertNull(dao.getConversation("other")?.subagentResultId)
    }

    @Test fun standaloneCompactSettlesWithoutOutboxAndFollowingOrdinaryResultCommitsOnce() = runTest {
        seed()
        repository.createSubagent(identity, ChatEntity("child", "Worker", parentConversationId = "parent"), null)
        var timestamp = 100L
        suspend fun compact(id: String) {
            val messageId = Constants.COMPACT_MSG_PREFIX + id
            val messages = input("child", id).map {
                it.copy(id = if (it.participant == Participant.MODEL) messageId else it.id,
                    timestamp = timestamp++)
            }
            repository.createRunWithMessages(run("child", id), messages, emptyMap(), null,
                at = 20L, touchConversationOnAdmission = false)
            val message = ChatMessage(id = messageId, text = "Summary", participant = Participant.MODEL,
                status = MessageStatus.SUCCESS, runId = id, runSequence = 1L)
            assertTrue(repository.finishGeneration(message, "child", id, RunStatus.COMPLETED,
                RunEndReason.MODEL_COMPLETED))
            assertEquals(RunStatus.COMPLETED, dao.getRun(id)?.status)
            assertEquals("Summary", dao.getMessage(messageId)?.text)
            assertTrue(repository.pendingSubagentResults("parent").isEmpty())
        }
        compact("before")
        assertNull(dao.getConversation("child")?.subagentResultId)
        repository.createRunWithMessages(run("child", "ordinary"), input("child", "ordinary").map {
            it.copy(timestamp = timestamp++)
        }, emptyMap(), null, at = 20L, touchConversationOnAdmission = false)
        val final = ChatMessage(id = "m-ordinary", text = "Answer", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = "ordinary", runSequence = 1L)
        assertTrue(repository.finishGeneration(final, "child", "ordinary", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        assertEquals("m-ordinary", repository.pendingSubagentResults("parent").single().resultId)
        assertFalse(repository.finishGeneration(final, "child", "ordinary", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        assertEquals(1, repository.pendingSubagentResults("parent").size)
        repository.consumeSubagentCompletions(identity, mapOf("child" to "m-ordinary"))
        val previous = checkNotNull(dao.getConversation("child"))
        compact("after")
        val after = checkNotNull(dao.getConversation("child"))
        assertEquals(previous.subagentResultId, after.subagentResultId)
        assertEquals(previous.subagentResultJson, after.subagentResultJson)
        assertEquals(previous.subagentDeliveredResultId, after.subagentDeliveredResultId)
    }

    @Test fun samePositionRecompactDoesNotRepublishLaterOrdinaryAnswerOnSuccessOrStop() = runTest {
        seed()
        repository.createSubagent(identity, ChatEntity("child", "Worker", parentConversationId = "parent"), null)
        admit("child", "initial")
        val initial = ChatMessage(id = "m-initial", text = "Initial answer", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = "initial", runSequence = 1L)
        assertTrue(repository.finishGeneration(initial, "child", "initial", RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        val compactId = Constants.COMPACT_MSG_PREFIX + "middle"
        val compactRun = run("child", "compact").copy(parentRunId = "initial")
        val compactMessage = MessageEntity(id = compactId, conversationId = "child", parentId = initial.id,
            text = "", participant = Participant.MODEL, status = MessageStatus.SENDING,
            timestamp = 10L, runId = compactRun.id, runSequence = 0L)
        repository.createRunWithMessages(compactRun, listOf(compactMessage), emptyMap(), null,
            at = 11L, touchConversationOnAdmission = false)
        val summary = ChatMessage(id = compactId, text = "Summary", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = compactRun.id, runSequence = 0L)
        assertTrue(repository.finishGeneration(summary, "child", compactRun.id, RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        val suffixRun = run("child", "suffix").copy(parentRunId = compactRun.id)
        val suffixMessages = input("child", suffixRun.id).map {
            it.copy(parentId = if (it.participant == Participant.USER) compactId else it.parentId,
                timestamp = it.timestamp + 20L)
        }
        repository.createRunWithMessages(suffixRun, suffixMessages, emptyMap(), null,
            at = 23L, touchConversationOnAdmission = false)
        val suffix = ChatMessage(id = "m-suffix", text = "Later answer", participant = Participant.MODEL,
            status = MessageStatus.SUCCESS, runId = suffixRun.id, runSequence = 1L)
        assertTrue(repository.finishGeneration(suffix, "child", suffixRun.id, RunStatus.COMPLETED,
            RunEndReason.MODEL_COMPLETED))
        repository.consumeSubagentCompletions(identity, mapOf("child" to suffix.id))
        val childBefore = checkNotNull(dao.getConversation("child"))
        val suffixBefore = dao.getMessage(suffix.id)
        listOf(false, true).forEachIndexed { index, stopped ->
            val replacement = run("child", "recompact-$index").copy(parentRunId = "initial")
            repository.beginRecompactContextCompact(replacement, compactId, "Provider:model", emptyMap())
            assertEquals(suffix.id, dao.getLastMessageForConversation("child")?.id)
            val output = summary.copy(runId = replacement.id, text = "New summary")
            if (stopped) assertTrue(repository.finishStoppedGeneration(listOf(output), replacement.id))
            else assertTrue(repository.finishGeneration(output, "child", replacement.id, RunStatus.COMPLETED,
                RunEndReason.MODEL_COMPLETED))
            val after = checkNotNull(dao.getConversation("child"))
            assertEquals(childBefore.subagentResultId, after.subagentResultId)
            assertEquals(childBefore.subagentResultJson, after.subagentResultJson)
            assertEquals(childBefore.subagentDeliveredResultId, after.subagentDeliveredResultId)
            assertEquals(suffixBefore, dao.getMessage(suffix.id))
            assertTrue(repository.pendingSubagentResults("parent").isEmpty())
        }
    }

    @Test fun waitToolRoundAcknowledgesOnlyWhenProtocolRowsCommitAndRejectsStalePass() = runTest {
        seed()
        child()
        fun round(prefix: String, result: String): List<MessageEntity> {
            val call = MessageSegment(type = "tool", toolName = "wait_agents", toolArgs = "{}", toolCallId = prefix)
            return listOf(
                MessageEntity(id = "$prefix-tool", conversationId = "parent", text = "", participant = Participant.MODEL,
                    timestamp = 2L, runId = "r-parent", runSequence = 0L, toolCallJson = Json.encodeToString(listOf(call))),
                MessageEntity(id = "$prefix-result", conversationId = "parent", parentId = "$prefix-tool", text = "",
                    participant = Participant.USER, timestamp = 3L, runId = "r-parent", runSequence = 1L,
                    toolCallJson = Json.encodeToString(listOf(call.copy(toolResult = result)))),
            )
        }
        val payload = """{"subagent_receipts":{"child":"result-child"},"results":[]}"""
        val stale = round("stale", payload)
        assertTrue(runCatching { repository.appendToolRoundToRun(stale, 1) }.isFailure)
        assertNull(dao.getConversation("child")?.subagentDeliveredResultId)
        val invalid = round("invalid", """{"subagent_receipts":{"child":"replaced"}}""")
        assertTrue(runCatching { repository.appendToolRoundToRun(invalid, 0) }.isFailure)
        assertNull(dao.getMessage("invalid-tool"))
        assertNull(dao.getConversation("child")?.subagentDeliveredResultId)
        val accepted = round("accepted", payload)
        assertTrue(repository.appendToolRoundToRun(accepted, 0).inserted)
        assertEquals("result-child", dao.getConversation("child")?.subagentDeliveredResultId)
        assertFalse(repository.appendToolRoundToRun(accepted, 0).inserted)
        assertTrue(repository.pendingSubagentResults("parent").isEmpty())
    }

    @Test fun deletingParentRemovesChildrenGraphsButPreservesOtherOwners() = runTest {
        seed()
        child()
        admit("child", "child-run")
        admit("other", "other-run")
        val other = dao.getConversation("other")
        val otherMessage = dao.getMessage("u-other-run")
        val prepared = mutableListOf<String>()
        repository.prepareConversationDeletion = { prepared += it }
        repository.deleteConversation("parent")
        assertEquals(listOf("parent"), prepared)
        assertNull(dao.getConversation("parent"))
        assertNull(dao.getConversation("child"))
        assertNull(dao.getMessage("u-child-run"))
        assertNull(dao.getRun("child-run"))
        assertNull(dao.getConversationSettingsTransfer("child"))
        assertEquals(other, dao.getConversation("other"))
        assertEquals(otherMessage, dao.getMessage("u-other-run"))
    }
}
