package com.newoether.agora.viewmodel

import com.newoether.agora.automation.ConversationExecutionCoordinator
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.util.DebugLog
import io.mockk.Runs
import io.mockk.every
import io.mockk.just
import io.mockk.mockkObject
import io.mockk.unmockkObject
import io.mockk.coEvery
import io.mockk.mockk
import java.util.concurrent.atomic.AtomicBoolean
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.cancel
import kotlinx.coroutines.launch
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withContext
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class RunningConversationDeletionTest {
    @Test
    fun actualAdapterAndLifecycleWaitForBothCanonicalBarriersInEitherOrder() = runBlocking {
        for (selected in listOf("conversation", "other")) {
            for (persistenceFirst in listOf(true, false)) {
                withTimeout(10_000) {
                    val fixture = Fixture()
                    val state = fixture.registry.getOrCreate("conversation")
                    val unrelated = fixture.registry.getOrCreate("unrelated")
                    val owner = state.acquireForSend()!!
                    state.bindRun(owner, "run")
                    val unrelatedOwner = unrelated.acquireForSend()!!
                    val unwind = CompletableDeferred<Unit>()
                    val cancelled = CompletableDeferred<Unit>()
                    val settled = CompletableDeferred<Unit>()
                    val started = CompletableDeferred<Unit>()
                    val coordinator = ConversationExecutionCoordinator()
                    val generation = launch(Dispatchers.Default) {
                        coordinator.withConversationLock("conversation") {
                            started.complete(Unit)
                            try {
                                awaitCancellation()
                            } finally {
                                cancelled.complete(Unit)
                                withContext(NonCancellable) { unwind.await() }
                            }
                        }
                    }
                    assertTrue(state.attachGenerationJob(owner, generation))
                    started.await()
                    state.onQueueDrainRequested = { settled.complete(Unit) }
                    fixture.client.open = selected
                    val result = CompletableDeferred<Boolean>()
                    val controller = ConversationLifecycleController(
                        conversations = fixture.repository,
                        scope = this,
                        clients = fixture.clients,
                        stopLoop = {},
                        tryWithConversationLock = { id, block -> coordinator.tryWithConversationLock(id, block) },
                        removeRuntime = fixture.registry::remove,
                        stopGeneration = fixture.adapter::stopAndAwait,
                        deletedElsewhereText = { "Deleted elsewhere" },
                        mainDispatcher = Dispatchers.Default,
                    )
                    try {
                        assertTrue(controller.delete(fixture.client, "conversation", onResult = { result.complete(it) }))
                        fixture.writeStarted.await()
                        cancelled.await()
                        assertFalse(result.isCompleted)
                        assertFalse(fixture.deleted.get())
                        if (persistenceFirst) {
                            fixture.persisted.complete(true)
                            assertTrue(state.generating.value)
                            assertFalse(fixture.deleted.get())
                            unwind.complete(Unit)
                        } else {
                            unwind.complete(Unit)
                            generation.join()
                            assertTrue(state.generating.value)
                            assertFalse(fixture.deleted.get())
                            fixture.persisted.complete(true)
                        }
                        settled.await()
                        assertTrue(result.await())
                        assertTrue(generation.isCompleted)
                        assertTrue(fixture.deleted.get())
                        assertTrue(unrelated.isCurrentToken(unrelatedOwner))
                        assertTrue(unrelated.generating.value)
                    } finally {
                        unwind.complete(Unit)
                        fixture.persisted.complete(true)
                        generation.cancel()
                        fixture.registry.cancelAll()
                    }
                }
            }
        }
    }

    @Test
    fun actualAdapterReportsFailedPersistenceInsteadOfReleasingStopOrDeleting() = runBlocking {
        withTimeout(10_000) {
            mockkObject(DebugLog)
            every { DebugLog.e(any(), any()) } just Runs
            val fixture = Fixture()
            val state = fixture.registry.getOrCreate("conversation")
            val owner = state.acquireForSend()!!
            state.bindRun(owner, "run")
            val pending = async { runCatching { fixture.adapter.stopAndAwait("conversation", fixture.client) } }
            try {
                fixture.writeStarted.await()
                assertFalse(pending.isCompleted)
                fixture.persisted.complete(false)
                assertTrue(pending.await().isFailure)
                assertTrue(state.generating.value)
                assertTrue(state.stopping.value)
                assertFalse(fixture.deleted.get())
            } finally {
                fixture.persisted.complete(false)
                fixture.registry.cancelAll()
                unmockkObject(DebugLog)
            }
        }
    }

    @Test
    fun duplicateStopFailsClosedBeforeAndAfterTheOriginalFinalizerFails() = runBlocking {
        withTimeout(10_000) {
            mockkObject(DebugLog)
            every { DebugLog.e(any(), any()) } just Runs
            val fixture = Fixture()
            val state = fixture.registry.getOrCreate("conversation")
            val owner = state.acquireForSend()!!
            state.bindRun(owner, "run")
            val original = async { runCatching { fixture.adapter.stopAndAwait("conversation", fixture.client) } }
            try {
                fixture.writeStarted.await()
                // No second finalization effect exists, so a duplicate cannot await its result.
                assertTrue(runCatching { fixture.adapter.stopAndAwait("conversation", fixture.client) }.isFailure)
                assertFalse(original.isCompleted)
                assertTrue(state.stopping.value)

                fixture.persisted.complete(false)
                assertTrue(original.await().isFailure)
                assertTrue(runCatching { fixture.adapter.stopAndAwait("conversation", fixture.client) }.isFailure)
                assertTrue(state.generating.value)
                assertTrue(state.stopping.value)
                assertFalse(fixture.deleted.get())
                val firstResult = CompletableDeferred<Boolean>()
                val controller = fixture.controller(this)
                assertTrue(controller.delete(fixture.client, "conversation", onResult = { firstResult.complete(it) }))
                assertFalse(firstResult.await())
                // Failure releases pending-delete admission, so a new confirmation can retry.
                val secondResult = CompletableDeferred<Boolean>()
                assertTrue(controller.delete(fixture.client, "conversation", onResult = { secondResult.complete(it) }))
                assertFalse(secondResult.await())
            } finally {
                fixture.persisted.complete(false)
                fixture.registry.cancelAll()
                unmockkObject(DebugLog)
            }
        }
    }

    private class Fixture {
        val repository = mockk<ConversationRepository>()
        val registry = ConversationStateRegistry {}
        val client = FakeChatClient(open = "conversation")
        val clients = ChatClients().also { it.attach(client) }
        val writeStarted = CompletableDeferred<Unit>()
        val persisted = CompletableDeferred<Boolean>()
        val deleted = AtomicBoolean(false)
        val adapter = GenerationStopAdapter(registry, clients, GenerationFinalizer(repository) { _, _ -> }) { "Failed" }

        fun controller(scope: CoroutineScope): ConversationLifecycleController {
            val coordinator = ConversationExecutionCoordinator()
            return ConversationLifecycleController(
                conversations = repository,
                scope = scope,
                clients = clients,
                stopLoop = {},
                tryWithConversationLock = { id, block -> coordinator.tryWithConversationLock(id, block) },
                removeRuntime = registry::remove,
                stopGeneration = adapter::stopAndAwait,
                deletedElsewhereText = { "Deleted elsewhere" },
                mainDispatcher = Dispatchers.Default,
            )
        }

        init {
            coEvery { repository.requestRunStop("run", any()) } returns true
            coEvery { repository.finishStoppedGeneration(any(), "run", any()) } coAnswers {
                writeStarted.complete(Unit)
                persisted.await()
            }
            coEvery { repository.getLiveRun("conversation") } returns null
            coEvery { repository.getConversation("conversation") } returns null
            coEvery { repository.deleteConversation("conversation") } answers { deleted.set(true); Unit }
        }
    }
}
