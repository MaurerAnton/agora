package com.newoether.agora.viewmodel

import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.automation.ConversationExecutionCoordinator
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.mockk
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(kotlinx.coroutines.ExperimentalCoroutinesApi::class)
class ConversationLifecycleControllerTest {
    @Test
    fun admittedDeletionHoldsTheCoordinatorUntilStorageCompletes() = runTest {
        val fixture = Fixture(this, controllerScope = backgroundScope)
        val releaseDeletion = CompletableDeferred<Unit>()
        coEvery { fixture.conversations.deleteConversation("conversation") } coAnswers {
            fixture.events += "delete-start"
            releaseDeletion.await()
            fixture.events += "delete-end"
        }
        fixture.controller.delete("conversation")
        runCurrent()
        backgroundScope.launch {
            fixture.executionCoordinator.withAutomationConversationLock("conversation") {
                fixture.events += "generation"
            }
        }
        runCurrent()
        assertTrue("delete-start" in fixture.events)
        assertTrue("generation" !in fixture.events)

        releaseDeletion.complete(Unit)
        runCurrent()

        assertTrue(fixture.events.indexOf("generation") > fixture.events.indexOf("delete-end"))
        coVerify(exactly = 1) { fixture.conversations.deleteConversation("conversation") }
    }

    @Test
    fun staleConfirmationIsRejectedBeforeStoppingGenerationOrDeletingStorage() = runTest {
        val fixture = Fixture(this)
        coEvery { fixture.conversations.getMessageTopologySnapshot("conversation") } returns emptyList()
        val results = mutableListOf<Boolean>()

        fixture.controller.delete("conversation", setOf("confirmed-message"), results::add)
        runCurrent()

        assertEquals(listOf(false), results)
        assertTrue("abort:7" in fixture.events)
        assertTrue("stop" !in fixture.events && "stop-loop" !in fixture.events)
        coVerify(exactly = 0) { fixture.conversations.deleteConversation(any()) }
    }

    @Test
    fun generationStartedBeforeDeleteAdmissionIsRejectedWithoutWaitingForItsCompletion() = runTest {
        val fixture = Fixture(this, controllerScope = backgroundScope)
        val releaseGeneration = CompletableDeferred<Unit>()
        val generation = backgroundScope.launch {
            fixture.executionCoordinator.withAutomationConversationLock("conversation") {
                releaseGeneration.await()
            }
        }
        runCurrent()
        val results = mutableListOf<Boolean>()

        assertTrue(fixture.controller.delete("conversation", onResult = results::add))
        runCurrent()

        assertEquals(listOf(false), results)
        assertTrue(generation.isActive)
        assertTrue("abort:7" in fixture.events)
        assertTrue("stop" !in fixture.events && "stop-loop" !in fixture.events)
        coVerify(exactly = 0) { fixture.conversations.deleteConversation(any()) }
        releaseGeneration.complete(Unit)
    }

    @Test
    fun cancelledDeletionCompletesItsFailureCallbackEvenWhileTheScopeIsCancelled() = runTest {
        val controllerJob = SupervisorJob()
        val controllerScope = CoroutineScope(StandardTestDispatcher(testScheduler) + controllerJob)
        val fixture = Fixture(this, controllerScope = controllerScope)
        fixture.onOverlay = { CompletableDeferred<Unit>().await() }
        val results = mutableListOf<Boolean>()
        fixture.controller.delete("conversation", onResult = results::add)
        runCurrent()

        controllerJob.cancel()
        runCurrent()

        assertEquals(listOf(false), results)
        assertTrue("abort:null" in fixture.events)
        coVerify(exactly = 0) { fixture.conversations.deleteConversation(any()) }
    }

    @Test
    fun renamePersistsTheExactTitle() = runTest {
        val fixture = Fixture(this)
        coEvery { fixture.conversations.updateConversationTitle(any(), any()) } returns true

        fixture.controller.rename("conversation", "New title")
        runCurrent()

        coVerify(exactly = 1) {
            fixture.conversations.updateConversationTitle("conversation", "New title")
        }
    }

    @Test
    fun visibleDeletionPreservesStopLockCleanupAndSelectionSettlementOrder() = runTest {
        val fixture = Fixture(this)
        coEvery { fixture.conversations.deleteConversation("conversation") } answers {
            fixture.events += "delete"
        }

        fixture.controller.delete("conversation")
        assertTrue(fixture.events.isEmpty())
        runCurrent()

        assertEquals(
            listOf(
                "overlay:conversation",
                "lock-start",
                "stop",
                "stop-loop",
                "delete",
                "lock-end",
                "remove",
                "settle:conversation",
            ),
            fixture.events,
        )
    }

    @Test
    fun deletionDoesNotStopOrReplaceAnotherVisibleConversation() = runTest {
        val fixture = Fixture(this, currentConversationId = "other")
        coEvery { fixture.conversations.deleteConversation("conversation") } answers {
            fixture.events += "delete"
        }

        fixture.controller.delete("conversation")
        runCurrent()

        assertEquals(listOf("lock-start", "stop-loop", "delete", "lock-end", "remove"), fixture.events)
        assertTrue("stop" !in fixture.events)
        assertTrue(fixture.events.none { it.startsWith("settle:") })
    }

    @Test
    fun selectionChangeBeforeFinalLockDoesNotStopTheNewVisibleConversation() = runTest {
        val fixture = Fixture(this)
        coEvery { fixture.conversations.deleteConversation("conversation") } answers {
            fixture.events += "delete"
        }
        fixture.onLockStart = { fixture.currentConversationId.value = "other" }

        fixture.controller.delete("conversation")
        runCurrent()

        assertTrue("stop" !in fixture.events)
        assertTrue(fixture.events.none { it.startsWith("settle:") })
        assertTrue("delete" in fixture.events)
    }

    @Test
    fun lifecycleSettlesTheOriginallySelectedDeletionAfterCleanup() = runTest {
        val fixture = Fixture(this)
        coEvery { fixture.conversations.deleteConversation("conversation") } answers {
            fixture.events += "delete"
        }
        fixture.onRemove = { fixture.currentConversationId.value = "other" }

        fixture.controller.delete("conversation")
        runCurrent()

        assertTrue("stop" in fixture.events)
        assertEquals("settle:conversation", fixture.events.last())
    }

    @Test
    fun frozenSubmissionRejectsDeletionBeforeAnySideEffect() = runTest {
        val fixture = Fixture(this, deleteLocked = true)

        assertFalse(fixture.controller.delete("conversation"))
        runCurrent()

        assertTrue(fixture.events.isEmpty())
        coVerify(exactly = 0) { fixture.conversations.deleteConversation(any()) }
    }

    @Test
    fun failedDurableDeletionDoesNotCleanupOrSettleSelection() = runTest {
        val failures = mutableListOf<Throwable>()
        val controllerScope = CoroutineScope(
            SupervisorJob() +
                StandardTestDispatcher(testScheduler) +
                CoroutineExceptionHandler { _, error -> failures += error },
        )
        val fixture = Fixture(this, controllerScope = controllerScope)
        coEvery { fixture.conversations.deleteConversation("conversation") } throws
            IllegalStateException("delete failed")

        fixture.controller.delete("conversation")
        runCurrent()

        assertTrue(failures.isEmpty())
        assertTrue("remove" !in fixture.events)
        assertTrue(fixture.events.none { it.startsWith("settle:") })
        assertTrue("abort:7" in fixture.events)
    }

    private class Fixture(
        testScope: kotlinx.coroutines.test.TestScope,
        currentConversationId: String? = "conversation",
        controllerScope: CoroutineScope = testScope,
        deleteLocked: Boolean = false,
    ) {
        val conversations = mockk<ConversationRepository>()
        val executionCoordinator = ConversationExecutionCoordinator()
        val currentConversationId = MutableStateFlow(currentConversationId)
        val events = mutableListOf<String>()
        var onRemove: () -> Unit = {}
        var onLockStart: () -> Unit = {}
        var onOverlay: suspend () -> Unit = {}
        private val dispatcher = StandardTestDispatcher(testScope.testScheduler)
        val controller = ConversationLifecycleController(
            currentConversationId = this.currentConversationId,
            conversations = conversations,
            scope = controllerScope,
            stopLoop = { events += "stop-loop" },
            tryWithConversationLock = { id, block ->
                executionCoordinator.tryWithConversationLock(id) {
                    events += "lock-start"
                    onLockStart()
                    block()
                    events += "lock-end"
                }
            },
            removeRuntime = {
                events += "remove"
                onRemove()
            },
            stopVisibleGeneration = { events += "stop" },
            settleDeletedSelectedConversation = { conversationId ->
                events += "settle:$conversationId"
            },
            beginSelectedDeleteTransition = { conversationId ->
                events += "overlay:$conversationId"
                onOverlay()
                7L
            },
            abortSelectedDeleteTransition = { requestId ->
                events += "abort:$requestId"
            },
            isDeleteLocked = { deleteLocked },
            ioDispatcher = dispatcher,
            mainDispatcher = dispatcher,
        )
    }
}
