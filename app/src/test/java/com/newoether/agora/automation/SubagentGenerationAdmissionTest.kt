package com.newoether.agora.automation

import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class SubagentGenerationAdmissionTest {
    @Test
    fun siblingsWait_otherFamiliesAndParentRemainIndependent_andSettingsWakeWaiters() = runTest {
        val limit = MutableStateFlow(1)
        val reads = mutableListOf<String>()
        val coordinator = ConversationExecutionCoordinator(
            parentConversationId = { id ->
                reads += id
                when (id) {
                    "a", "b", "c" -> "parent"
                    "other-child" -> "other-parent"
                    else -> null
                }
            },
            subagentMaxConcurrent = limit,
        )
        val releaseA = coordinator.acquireGenerationAdmission("a")
        val releaseB = CompletableDeferred<() -> Unit>()
        val waiting = launch { releaseB.complete(coordinator.acquireGenerationAdmission("b")) }
        runCurrent()
        assertFalse(releaseB.isCompleted)
        coordinator.withGenerationAdmission("other-child") {
            assertEquals(1, coordinator.admittedChildCount("other-parent"))
        }
        coordinator.withGenerationAdmission("parent") {
            assertEquals(1, coordinator.admittedChildCount("parent"))
        }
        limit.value = 2
        runCurrent()
        assertTrue(releaseB.isCompleted)
        assertEquals(2, coordinator.admittedChildCount("parent"))
        limit.value = 1
        val releaseC = CompletableDeferred<() -> Unit>()
        val next = launch { releaseC.complete(coordinator.acquireGenerationAdmission("c")) }
        runCurrent()
        assertFalse(releaseC.isCompleted)
        releaseA()
        runCurrent()
        assertFalse(releaseC.isCompleted)
        releaseB.await().invoke()
        runCurrent()
        assertTrue(releaseC.isCompleted)
        releaseC.await().invoke()
        waiting.join()
        next.join()
        assertEquals(0, coordinator.admittedChildCount("parent"))
        assertEquals(listOf("a", "b", "other-child", "parent", "c"), reads)
    }

    @Test
    fun nestedAndSeparatelyLaunchedJobsShareOneChildAllowance_withoutCoroutineInheritance() = runTest {
        val coordinator = ConversationExecutionCoordinator({ "parent" }, MutableStateFlow(1))
        val releaseWorker = coordinator.acquireGenerationAdmission("child")
        val releaseRun = CompletableDeferred<() -> Unit>()
        // This Job is a sibling of the worker; no coroutine-context lease is inherited.
        launch { releaseRun.complete(coordinator.acquireGenerationAdmission("child")) }.join()
        assertEquals(1, coordinator.admittedChildCount("parent"))
        releaseWorker()
        assertEquals(1, coordinator.admittedChildCount("parent"))
        releaseRun.await().invoke()
        releaseRun.await().invoke()
        assertEquals(0, coordinator.admittedChildCount("parent"))
    }

    @Test
    fun cancellationBeforeOrImmediatelyAfterWakeDoesNotLeakAllowance() = runTest {
        val coordinator = ConversationExecutionCoordinator({ "parent" }, MutableStateFlow(1))
        repeat(32) { index ->
            val release = coordinator.acquireGenerationAdmission("owner-$index")
            val waiting = launch {
                coordinator.withGenerationAdmission("waiting-$index") { error("cancelled waiter entered") }
            }
            runCurrent()
            if (index % 2 == 0) release()
            waiting.cancelAndJoin()
            release()
            assertEquals(0, coordinator.admittedChildCount("parent"))
        }
        coordinator.withGenerationAdmission("next") {
            assertEquals(1, coordinator.admittedChildCount("parent"))
        }
        assertEquals(0, coordinator.admittedChildCount("parent"))
    }

    @Test
    fun recoveryAndGraphLocksConsumeNoAllowance() = runTest {
        val coordinator = ConversationExecutionCoordinator(
            parentConversationId = { error("graph lock must not query parent") },
            subagentMaxConcurrent = MutableStateFlow(1),
        )
        coordinator.withConversationLock("child") {
            assertEquals(0, coordinator.admittedChildCount("parent"))
        }
        assertTrue(coordinator.tryWithConversationLock("child") {})
    }
}
