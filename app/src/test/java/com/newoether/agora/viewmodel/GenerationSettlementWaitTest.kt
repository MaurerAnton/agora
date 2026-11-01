package com.newoether.agora.viewmodel

import com.newoether.agora.model.ConversationCommand
import com.newoether.agora.model.RunEffectIdentity
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.Job
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class GenerationSettlementWaitTest {
    @Test
    fun durableSuccessWaitsForInstalledJob_andDoesNotFollowSuccessor() = runBlocking<Unit> {
        withTimeout(5_000) {
            val state = ConversationGenerationState("child", reclaimQueuedAttachments = {})
            try {
                val token = checkNotNull(state.tryAcquireForReplacement())
                val owner = Job()
                assertTrue(state.attachGenerationJob(token, owner))
                state.bindRun(token, "original-run")
                val settled = async(start = CoroutineStart.UNDISPATCHED) {
                    state.awaitGenerationSettled(token)
                }
                val effect = checkNotNull(state.commands.requestRunFinalization(
                    RunEffectIdentity("child", token, "original-run", 0, "terminal"),
                    RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED, false,
                ))
                state.finishRunFinalization(effect.identity, true)
                assertFalse(settled.isCompleted)
                owner.complete()
                owner.join()
                settled.await()
                val successor = checkNotNull(state.tryAcquireForReplacement())
                state.awaitGenerationSettled(token)
                assertTrue(state.generating.value)
                assertTrue(successor > token)
                state.endGeneration(successor)
            } finally {
                state.dispose()
            }
        }
    }

    @Test
    fun jobCompletionAndFailedPersistenceDoNotSettleTheOwner() = runBlocking<Unit> {
        withTimeout(5_000) {
            val state = ConversationGenerationState("child", reclaimQueuedAttachments = {})
            try {
                val token = checkNotNull(state.tryAcquireForReplacement())
                val owner = Job()
                assertTrue(state.attachGenerationJob(token, owner))
                state.bindRun(token, "original-run")
                val settled = async(start = CoroutineStart.UNDISPATCHED) {
                    state.awaitGenerationSettled(token)
                }
                owner.complete()
                owner.join()
                state.endGeneration(token)
                assertFalse(settled.isCompleted)
                val effect = checkNotNull(state.commands.requestRunFinalization(
                    RunEffectIdentity("child", token, "original-run", 0, "terminal"),
                    RunStatus.COMPLETED, RunEndReason.MODEL_COMPLETED, false,
                ))
                state.finishRunFinalization(effect.identity, false)
                assertFalse(settled.isCompleted)
                state.finishRunFinalization(effect.identity, true)
                settled.await()
            } finally {
                state.dispose()
            }
        }
    }

    @Test
    fun stopTokenRevocationAndFailedStopWriteDoNotSettleTheOwner() = runBlocking<Unit> {
        withTimeout(5_000) {
            val state = ConversationGenerationState("child", reclaimQueuedAttachments = {})
            try {
                val token = checkNotNull(state.tryAcquireForReplacement())
                val owner = Job()
                assertTrue(state.attachGenerationJob(token, owner))
                state.bindRun(token, "original-run")
                val settled = async(start = CoroutineStart.UNDISPATCHED) {
                    state.awaitGenerationSettled(token)
                }
                val stopped = state.stop()
                owner.join()
                state.endGeneration(token)
                assertFalse(state.isCurrentToken(token))
                assertFalse(settled.isCompleted)
                val identity = checkNotNull(stopped.finalizationEffect).identity
                state.finishStopFinalization(ConversationCommand.PersistenceSettled(identity, false))
                assertFalse(settled.isCompleted)
                state.finishStopFinalization(ConversationCommand.PersistenceSettled(identity, true))
                settled.await()
            } finally {
                state.dispose()
            }
        }
    }

    @Test
    fun cancelledWaiterDoesNotReleaseOrChangeTheGeneration() = runBlocking<Unit> {
        withTimeout(5_000) {
            val state = ConversationGenerationState("child", reclaimQueuedAttachments = {})
            try {
                val token = checkNotNull(state.tryAcquireForReplacement())
                val waiter = async(start = CoroutineStart.UNDISPATCHED) {
                    state.awaitGenerationSettled(token)
                }
                assertFalse(waiter.isCompleted)
                waiter.cancelAndJoin()
                assertTrue(state.generating.value)
                assertEquals(token, state.captureUiToken())
                state.endGeneration(token)
            } finally {
                state.dispose()
            }
        }
    }
}
