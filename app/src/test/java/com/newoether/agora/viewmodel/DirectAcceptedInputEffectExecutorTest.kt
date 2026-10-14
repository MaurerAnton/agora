package com.newoether.agora.viewmodel

import com.newoether.agora.automation.ConversationExecutionCoordinator
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.NewChatPersistEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunEffect
import com.newoether.agora.model.RunStatus
import io.mockk.Runs
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.just
import io.mockk.mockk
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNotNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class DirectAcceptedInputEffectExecutorTest {
    @Test
    fun durableCommitPrecedesAcceptanceProjectionAndBoundLaunch() = runBlocking {
        val fixture = Fixture()
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        val graphRequest = io.mockk.slot<AcceptedInputGraphWriter.Request>()
        coEvery { fixture.graphWriter.commit(capture(graphRequest), any()) } coAnswers {
            fixture.events += "room-commit"
            fixture.commit
        }
        coEvery { fixture.boundLauncher.launch(any(), state) } coAnswers {
            fixture.events += "bound-launch"
        }

        val execution = fixture.executor.launch(fixture.request(effect), state)
        val accepted = execution.awaitAcceptance()
        assertNotNull(execution.job)
        execution.job?.join()

        assertEquals(SendAcceptance.Direct(USER_ID, CONVERSATION_ID), accepted)
        assertTrue(graphRequest.captured.touchConversationOnAdmission)
        assertEquals(
            listOf(
                "capture-snapshot",
                "room-commit",
                "persist-user:$USER_ID",
                "accept-callback:$USER_ID",
                "accept-event:$USER_ID",
                "scroll:$USER_ID",
                "bound-launch",
            ),
            fixture.events,
        )
        assertEquals(MODEL_ID, state.streamingMessage.value?.id)
        coVerify(exactly = 1) {
            fixture.boundLauncher.launch(
                match {
                    it.conversationId == CONVERSATION_ID &&
                        it.modelMessageId == MODEL_ID &&
                        it.runId == RUN_ID &&
                        it.uiToken == effect.identity.ownerToken &&
                        it.persistId > 0 &&
                        it.pass == 0 &&
                        it.requestKind == "chat" &&
                        it.snapshot === fixture.snapshot
                },
                state,
            )
        }
        state.dispose()
        Unit
    }

    @Test
    fun uncommittedFailureReturnsNullAndDoesNotLaunchProvider() = runBlocking {
        val fixture = Fixture()
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        coEvery { fixture.graphWriter.commit(any(), any()) } throws
            IllegalStateException("Room unavailable")
        coEvery { fixture.conversations.getRun(RUN_ID) } returns null
        coEvery {
            fixture.terminalSettlement.failGenerationSetup(
                CONVERSATION_ID,
                RUN_ID,
                MODEL_ID,
                effect.identity.ownerToken,
                state,
                any(),
            )
        } just Runs

        val execution = fixture.executor.launch(fixture.request(effect), state)
        assertNull(execution.awaitAcceptance())
        execution.job?.join()

        assertTrue(fixture.events.none { it.startsWith("accept") })
        coVerify(exactly = 0) { fixture.boundLauncher.launch(any(), any()) }
        state.dispose()
        Unit
    }

    @Test
    fun cancellationAfterDurableCommitReconcilesIdentity() = runBlocking {
        val fixture = Fixture()
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        coEvery { fixture.graphWriter.commit(any(), any()) } coAnswers {
            fixture.events += "room-commit"
            throw CancellationException("cancelled after Room commit")
        }
        coEvery { fixture.conversations.getRun(RUN_ID) } returns ACTIVE_RUN
        coEvery {
            fixture.terminalSettlement.settleCancelledDurableRun(state, any())
        } returns true

        val execution = fixture.executor.launch(
            fixture.request(
                effect = effect,
                wasNewChat = true,
                newConversation = ChatEntity(CONVERSATION_ID, "New chat"),
                newConversationSettings = CAPTURED_SETTINGS,
            ),
            state,
        )
        val accepted = execution.awaitAcceptance()
        execution.job?.join()

        assertEquals(SendAcceptance.Direct(USER_ID, CONVERSATION_ID), accepted)
        val commitIndex = fixture.events.indexOf("room-commit")
        val transferIndex = fixture.events.indexOf(
            "apply-committed:$CONVERSATION_ID",
        )
        assertTrue(commitIndex >= 0)
        assertTrue(transferIndex > commitIndex)
        coVerify(exactly = 1) {
            fixture.terminalSettlement.settleCancelledDurableRun(
                state,
                ConversationGenerationState.RunBindingOutcome.Active,
            )
        }
        coVerify(exactly = 0) {
            fixture.terminalSettlement.failGenerationSetup(any(), any(), any(), any(), any(), any())
        }
        state.dispose()
        Unit
    }

    @Test
    fun newConversationPublishesOnlyAfterDurableCommit() = runBlocking {
        val fixture = Fixture()
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        val graphRequest = io.mockk.slot<AcceptedInputGraphWriter.Request>()
        coEvery { fixture.graphWriter.commit(capture(graphRequest), any()) } coAnswers {
            fixture.events += "room-commit"
            fixture.commit
        }
        coEvery { fixture.boundLauncher.launch(any(), state) } just Runs

        val execution = fixture.executor.launch(
            fixture.request(
                effect = effect,
                wasNewChat = true,
                newConversation = ChatEntity(CONVERSATION_ID, "New chat"),
                newConversationSettings = CAPTURED_SETTINGS,
                newChatPersistSnapshot = NEW_CHAT_PERSIST,
            ),
            state,
        )
        execution.awaitAcceptance()
        execution.job?.join()

        val commitIndex = fixture.events.indexOf("room-commit")
        val transferIndex = fixture.events.indexOf(
            "apply-committed:$CONVERSATION_ID",
        )
        val acceptanceIndex = fixture.events.indexOf("accept-callback:$USER_ID")
        val publicationIndex = fixture.events.indexOf("publish-new:provider:model")
        val presentationIndex = fixture.events.indexOf("accept-event:$USER_ID")
        assertTrue(commitIndex >= 0)
        assertTrue(transferIndex > commitIndex)
        assertTrue(acceptanceIndex > transferIndex)
        assertTrue(publicationIndex > acceptanceIndex)
        assertTrue(presentationIndex > publicationIndex)
        assertEquals(NEW_CHAT_PERSIST, graphRequest.captured.newChatPersistSnapshot)
        state.dispose()
        Unit
    }

    @Test
    fun outboxTransferFailureDoesNotRejectTheDurableSend() = runBlocking {
        val fixture = Fixture(
            applyCommittedError = IllegalStateException("DataStore unavailable"),
        )
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        coEvery { fixture.graphWriter.commit(any(), any()) } coAnswers {
            fixture.events += "room-commit"
            fixture.commit
        }
        coEvery { fixture.boundLauncher.launch(any(), state) } coAnswers {
            fixture.events += "bound-launch"
        }

        val execution = fixture.executor.launch(
            fixture.request(
                effect = effect,
                wasNewChat = true,
                newConversation = ChatEntity(CONVERSATION_ID, "New chat"),
                newConversationSettings = CAPTURED_SETTINGS,
            ),
            state,
        )
        val accepted = execution.awaitAcceptance()
        execution.job?.join()

        assertEquals(SendAcceptance.Direct(USER_ID, CONVERSATION_ID), accepted)
        assertTrue(fixture.events.contains("apply-committed:$CONVERSATION_ID"))
        assertTrue(fixture.events.contains("accept-callback:$USER_ID"))
        assertTrue(fixture.events.contains("publish-new:provider:model"))
        assertTrue(fixture.events.contains("accept-event:$USER_ID"))
        assertTrue(fixture.events.contains("bound-launch"))
        coVerify(exactly = 0) {
            fixture.terminalSettlement.failGenerationSetup(any(), any(), any(), any(), any(), any())
        }
        state.dispose()
        Unit
    }

    @Test
    fun frozenAdmissionSnapshotIsUsedWithoutRecapturingMutableSettings() = runBlocking {
        val fixture = Fixture()
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        coEvery { fixture.graphWriter.commit(any(), any()) } returns fixture.commit
        coEvery { fixture.boundLauncher.launch(any(), state) } just Runs

        val execution = fixture.executor.launch(
            fixture.request(effect, generationSnapshot = fixture.snapshot),
            state,
        )
        assertNotNull(execution.awaitAcceptance())
        execution.job?.join()

        assertTrue("capture-snapshot" !in fixture.events)
        coVerify(exactly = 0) {
            fixture.requestBuilder.captureAdmissionSnapshot(
                any(), any(), any(), any(), any(), any(),
            )
        }
        coVerify(exactly = 1) {
            fixture.boundLauncher.launch(match { it.snapshot === fixture.snapshot }, state)
        }
        state.dispose()
        Unit
    }

    @Test
    fun staleNewChatAcceptanceDoesNotPublishPresentationOrScroll() = runBlocking {
        val fixture = Fixture(
            selectNewConversation = false,
            conversationOpen = false,
        )
        val state = ConversationGenerationState(CONVERSATION_ID)
        val effect = claimDirectEffect(state)
        coEvery { fixture.graphWriter.commit(any(), any()) } returns fixture.commit
        coEvery { fixture.boundLauncher.launch(any(), state) } just Runs

        val execution = fixture.executor.launch(
            fixture.request(
                effect = effect,
                wasNewChat = true,
                newConversation = ChatEntity(CONVERSATION_ID, "New chat"),
                newConversationSettings = CAPTURED_SETTINGS,
            ),
            state,
        )
        assertEquals(
            SendAcceptance.Direct(USER_ID, CONVERSATION_ID),
            execution.awaitAcceptance(),
        )
        execution.job?.join()

        assertTrue(fixture.events.contains("publish-new:provider:model"))
        assertTrue(fixture.events.none { it.startsWith("accept-event:") })
        assertTrue(fixture.events.none { it.startsWith("scroll:") })
        state.dispose()
        Unit
    }

    private class Fixture(
        private val applyCommittedError: Exception? = null,
        private val selectNewConversation: Boolean = true,
        private val conversationOpen: Boolean = true,
    ) {
        val conversations = mockk<ConversationRepository>()
        val settings = mockk<SettingsRepository>()
        val graphWriter = mockk<AcceptedInputGraphWriter>()
        val requestBuilder = mockk<GenerationRequestBuilder>()
        val terminalSettlement = mockk<GenerationTerminalSettlementController>()
        val boundLauncher = mockk<BoundRunGenerationLauncher>()
        val events = mutableListOf<String>()
        val snapshot = testGenerationAdmissionSnapshot(
            conversationId = CONVERSATION_ID,
            runId = RUN_ID,
        ).copy(titleGenerationEnabled = false)
        val commit = AcceptedInputGraphWriter.Commit(
            userMessage = USER_ENTITY,
            modelMessage = MODEL_ENTITY,
            messageSelections = mapOf(null to USER_ID, USER_ID to MODEL_ID),
        )
        private val ids = ArrayDeque(listOf(USER_ID, MODEL_ID))
        val executor: DirectAcceptedInputEffectExecutor

        init {
            coEvery { settings.incrementMessagesSent() } just Runs
            coEvery {
                requestBuilder.captureAdmissionSnapshot(
                    any(), any(), any(), any(), any(), any(),
                )
            } coAnswers {
                events += "capture-snapshot"
                snapshot
            }
            coEvery { conversations.getMessage(MODEL_ID) } returns
                MODEL_ENTITY.copy(status = MessageStatus.SUCCESS)

            executor = DirectAcceptedInputEffectExecutor(
                conversations = conversations,
                settings = settings,
                executionCoordinator = ConversationExecutionCoordinator(),
                graphWriter = graphWriter,
                renderStore = ConversationRenderStore(),
                requestBuilder = requestBuilder,
                terminalSettlement = terminalSettlement,
                boundRunGenerationLauncher = boundLauncher,
                acceptanceNotifier = SendAcceptanceNotifier { _, messageId ->
                    events += "accept-event:$messageId"
                },
                toUiMessage = ::toUiMessage,
                isConversationOpen = { conversationOpen },
                applyCommittedNewConversationState = { conversationId ->
                    events += "apply-committed:$conversationId"
                    applyCommittedError?.let { throw it }
                },
                publishNewConversation = { _, modelId, _ ->
                    events += "publish-new:$modelId"
                    selectNewConversation
                },
                onUserMessagePersisted = { messageId, _ ->
                    events += "persist-user:$messageId"
                },
                onGenerateTitle = { events += "generate-title" },
                idFactory = ids::removeFirst,
                clock = { 100L },
            )
        }

        fun request(
            effect: RunEffect.PersistAcceptedInput,
            wasNewChat: Boolean = false,
            newConversation: ChatEntity? = null,
            newConversationSettings: ConversationSettings? = null,
            newChatPersistSnapshot: NewChatPersistEntity? = null,
            touchConversationOnAdmission: Boolean = true,
            generationSnapshot: GenerationAdmissionSnapshot? = null,
        ) = DirectAcceptedInputRequest(
            inputEffect = effect,
            wasNewChat = wasNewChat,
            newConversation = newConversation,
            userText = "hello",
            payload = PAYLOAD,
            modelId = "provider:model",
            requestKind = "chat",
            touchConversationOnAdmission = touchConversationOnAdmission,
            newConversationSettings = newConversationSettings,
            newChatPersistSnapshot = newChatPersistSnapshot,
            alreadyHoldsLock = false,
            requestScroll = { _, messageId -> events += "scroll:$messageId" },
            onAccepted = { events += "accept-callback:${it.messageId}" },
            onModelMessageCreated = null,
            generationSnapshot = generationSnapshot,
            originNewChatEntryId = ENTRY_ID.takeIf { wasNewChat },
        )
    }

    private companion object {
        const val CONVERSATION_ID = "conversation"
        const val RUN_ID = "run"
        const val USER_ID = "user"
        const val MODEL_ID = "model"
        const val ENTRY_ID = 7L
        val CAPTURED_SETTINGS = ConversationSettings(temperature = 0.25f)
        val NEW_CHAT_PERSIST = NewChatPersistEntity(
            modelId = "provider:model",
            draftText = "hello",
        )
        val PAYLOAD = MessagePayloadBuilder.MessagePayload(
            allImages = listOf("image"),
            attachmentMeta = null,
        )
        val USER_ENTITY = MessageEntity(
            id = USER_ID,
            conversationId = CONVERSATION_ID,
            text = "hello",
            participant = Participant.USER,
            timestamp = 100L,
            runId = RUN_ID,
            runSequence = 0,
            consumedAtPass = 0,
        )
        val MODEL_ENTITY = MessageEntity(
            id = MODEL_ID,
            conversationId = CONVERSATION_ID,
            parentId = USER_ID,
            text = "",
            participant = Participant.MODEL,
            status = MessageStatus.SENDING,
            timestamp = 101L,
            modelName = "provider:model",
            runId = RUN_ID,
            runSequence = 1,
        )
        val ACTIVE_RUN = RunEntity(
            id = RUN_ID,
            conversationId = CONVERSATION_ID,
            parentRunId = null,
            status = RunStatus.ACTIVE,
            activeSlot = 1,
            startedAt = 100L,
            lastCheckpointAt = 101L,
        )
    }
}

private suspend fun claimDirectEffect(
    state: ConversationGenerationState,
): RunEffect.PersistAcceptedInput = state.commands.requestSend(
    proposedRunId = "run",
    effectId = "send-run",
    directOnly = false,
    hasPendingGuidance = false,
).effects.filterIsInstance<RunEffect.PersistAcceptedInput>().single()

private fun toUiMessage(entity: MessageEntity) = ChatMessage(
    id = entity.id,
    parentId = entity.parentId,
    text = entity.text,
    images = entity.images,
    thoughts = entity.thoughts,
    thoughtTitle = entity.thoughtTitle,
    status = entity.status,
    participant = entity.participant,
    timestamp = entity.timestamp,
    modelName = entity.modelName,
    runId = entity.runId,
    runSequence = entity.runSequence,
    consumedAtPass = entity.consumedAtPass,
)
