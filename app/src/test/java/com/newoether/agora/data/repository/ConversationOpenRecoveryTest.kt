package com.newoether.agora.data.repository

import com.newoether.agora.automation.ConversationExecutionCoordinator
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.util.DebugLog
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.coVerifyOrder
import io.mockk.confirmVerified
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkObject
import io.mockk.unmockkObject
import io.mockk.verify
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class ConversationOpenRecoveryTest {
    private val dao = mockk<ChatDao> {
        coEvery { getLiveRun(any()) } returns null
        coEvery { getValidatedSubagentStatus(any()) } returns null
    }
    private val repository = ConversationRepository(dao, database = null)
    private val coordinator = ConversationExecutionCoordinator()

    @Test
    fun constructionDoesNoOwnerWork() {
        confirmVerified(dao)
        assertFalse(coordinator.isExecuting("parent"))
    }

    @Test
    fun parentOpenRecoversSelectedOwnerThenOnlyIndexedChildren() = runTest {
        family("parent", listOf("one", "two"))

        repository.recoverOpenedConversationFamily("parent", coordinator)

        coVerifyOrder {
            dao.recoverConversationRuntime("parent", any())
            dao.getSubagentIds("parent")
            dao.recoverConversationRuntime("one", any())
            dao.recoverConversationRuntime("two", any())
        }
        coVerify(exactly = 3) { dao.recoverConversationRuntime(any(), any()) }
        coVerify(exactly = 0) { dao.getConversation(any()) }
        coVerify(exactly = 0) { dao.pendingSubagentResults(any()) }
        coVerify(exactly = 0) { dao.setSubagentResult(any(), any(), any(), any(), any(), any()) }
        verify(exactly = 0) { dao.getAllConversations() }
    }

    @Test
    fun childOpenResolvesProvenParentAndDoesNotRecoverSelectedChildTwice() = runTest {
        family("parent", listOf("one", "two", "three"))

        repository.recoverOpenedConversationFamily("two", coordinator)

        coVerifyOrder {
            dao.recoverConversationRuntime("two", any())
            dao.getSubagentIds("parent")
            dao.recoverConversationRuntime("one", any())
            dao.recoverConversationRuntime("three", any())
        }
        coVerify(exactly = 1) { dao.recoverConversationRuntime("two", any()) }
        coVerify(exactly = 0) { dao.recoverConversationRuntime("parent", any()) }
        coVerify(exactly = 0) { dao.getSubagentIds("two") }
    }

    @Test
    fun changedOrUnprovenParentNeverWidensRecovery() = runTest {
        family("parent", listOf("one", "two"))
        coEvery { dao.getSubagentFamilyRootId("foreign") } returns "parent"

        repository.recoverOpenedConversationFamily("foreign", coordinator)

        coVerify(exactly = 1) { dao.recoverConversationRuntime("foreign", any()) }
        coVerify(exactly = 1) { dao.recoverConversationRuntime(any(), any()) }
    }

    @Test
    fun movedChildIsRecheckedBeforeOrdinaryRecovery() = runTest {
        family("parent", listOf("one", "two"))
        coEvery { dao.getSubagentFamilyRootId("one") } returns "other-parent"

        repository.recoverOpenedConversationFamily("parent", coordinator)

        coVerify(exactly = 0) { dao.recoverConversationRuntime("one", any()) }
        coVerify(exactly = 1) { dao.recoverConversationRuntime("two", any()) }
    }

    @Test
    fun activeProcessOwnerIsSkippedWithoutWaitingOrBlockingOtherChildren() = runTest {
        family("parent", listOf("one", "two"))
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        val active = launch {
            coordinator.withAutomationConversationLock("one") {
                entered.complete(Unit)
                release.await()
            }
        }
        entered.await()

        repository.recoverOpenedConversationFamily("parent", coordinator)

        assertTrue(active.isActive)
        coVerify(exactly = 0) { dao.recoverConversationRuntime("one", any()) }
        coVerify(exactly = 1) { dao.recoverConversationRuntime("two", any()) }
        release.complete(Unit)
        active.join()
    }

    @Test
    fun admittedDelegationJobIsProtectedEvenBetweenRunLocks() = runTest {
        family("parent", listOf("one", "two"))
        val admitted = Job()
        repository.subagentJobs.value = mapOf("one" to admitted)
        try {
            repository.recoverOpenedConversationFamily("parent", coordinator)

            assertTrue(admitted.isActive)
            coVerify(exactly = 0) { dao.recoverConversationRuntime("one", any()) }
            coVerify(exactly = 1) { dao.recoverConversationRuntime("two", any()) }
        } finally {
            admitted.cancel()
        }
    }

    @Test
    fun cancelledAndLazyAdmittedJobsRemainProtectedUntilManagerRemovesThem() = runTest {
        family("parent", listOf("lazy", "cleanup", "idle"))
        val lazyJob = backgroundScope.launch(start = kotlinx.coroutines.CoroutineStart.LAZY) {
            error("must never be started by validation")
        }
        val cleanupJob = Job().also { it.cancel() }
        repository.subagentJobs.value = mapOf("lazy" to lazyJob, "cleanup" to cleanupJob)
        try {
            repository.recoverOpenedConversationFamily("parent", coordinator)

            coVerify(exactly = 0) { dao.recoverConversationRuntime("lazy", any()) }
            coVerify(exactly = 0) { dao.recoverConversationRuntime("cleanup", any()) }
            coVerify(exactly = 1) { dao.recoverConversationRuntime("idle", any()) }
            assertFalse(lazyJob.isActive)
        } finally {
            lazyJob.cancel()
        }
    }

    @Test
    fun missingOrCyclicRootDoesNotEnumerateChildren() = runTest {
        coEvery { dao.recoverConversationRuntime("orphan", any()) } returns 0
        coEvery { dao.getSubagentFamilyRootId("orphan") } returns null

        repository.recoverOpenedConversationFamily("orphan", coordinator)

        coVerify(exactly = 1) { dao.recoverConversationRuntime("orphan", any()) }
        coVerify(exactly = 0) { dao.getSubagentIds(any()) }
    }

    @Test
    fun selectionReparentedDuringEnumerationDoesNotRepairOldFamily() = runTest {
        family("parent", listOf("selected", "sibling"))
        coEvery { dao.getSubagentFamilyRootId("selected") } returnsMany
            listOf("parent", "other-parent")

        repository.recoverOpenedConversationFamily("selected", coordinator)

        coVerify(exactly = 1) { dao.recoverConversationRuntime("selected", any()) }
        coVerify(exactly = 0) { dao.recoverConversationRuntime("sibling", any()) }
    }

    @Test
    fun cancellationStopsTheFamilyWalkAndReleasesCurrentOwner() = runTest {
        family("parent", listOf("one", "two"))
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        coEvery { dao.recoverConversationRuntime("one", any()) } coAnswers {
            entered.complete(Unit)
            release.await()
            0
        }
        val open = launch { repository.recoverOpenedConversationFamily("parent", coordinator) }
        entered.await()

        open.cancelAndJoin()

        coVerify(exactly = 0) { dao.recoverConversationRuntime("two", any()) }
        assertTrue(coordinator.tryWithConversationLock("one") {})
        assertFalse(coordinator.isExecuting("one"))
    }

    @Test
    fun largeFamilyHasOneRecoveryInFlightAndOtherSelectedOwnersRemainIndependent() = runTest {
        val children = (1..512).map { "child-$it" }
        family("parent", children)
        family("unrelated", emptyList())
        val entered = CompletableDeferred<Unit>()
        val release = CompletableDeferred<Unit>()
        var inFlight = 0
        var maximum = 0
        var calls = 0
        coEvery { dao.recoverConversationRuntime(any(), any()) } coAnswers {
            inFlight += 1
            calls += 1
            maximum = maxOf(maximum, inFlight)
            if (firstArg<String>() == children.first()) {
                entered.complete(Unit)
                release.await()
            }
            inFlight -= 1
            0
        }
        val open = launch { repository.recoverOpenedConversationFamily("parent", coordinator) }
        entered.await()
        runCurrent()
        assertEquals(2, calls)
        assertEquals(1, maximum)
        assertFalse(coordinator.isExecuting("parent"))

        repository.recoverOpenedConversationFamily("unrelated", coordinator)
        coVerify(exactly = 1) { dao.recoverConversationRuntime("unrelated", any()) }
        release.complete(Unit)
        open.join()

        coVerify(exactly = 514) { dao.recoverConversationRuntime(any(), any()) }
        coVerify(exactly = 0) { dao.getConversation(any()) }
    }

    @Test
    fun childFailureDoesNotAbortRemainingValidationOrLeaveAnOwnerLock() = runTest {
        family("parent", listOf("one", "two"))
        coEvery { dao.recoverConversationRuntime("one", any()) } throws
            IllegalStateException("invalid child")
        mockkObject(DebugLog)
        every { DebugLog.e(any(), any(), any()) } returns Unit
        try {
            repository.recoverOpenedConversationFamily("parent", coordinator)

            coVerify(exactly = 1) { dao.recoverConversationRuntime("two", any()) }
            assertTrue(coordinator.tryWithConversationLock("one") {})
        } finally {
            unmockkObject(DebugLog)
        }
    }

    @Test
    fun selectedOwnerFailurePropagatesBeforeAnyFamilyEnumeration() = runTest {
        coEvery { dao.recoverConversationRuntime("parent", any()) } throws
            IllegalStateException("invalid selected owner")

        val failure = runCatching {
            repository.recoverOpenedConversationFamily("parent", coordinator)
        }.exceptionOrNull()

        assertTrue(failure is IllegalStateException)
        coVerify(exactly = 0) { dao.getSubagentIds(any()) }
        coVerify(exactly = 0) { dao.getSubagentFamilyRootId(any()) }
        assertTrue(coordinator.tryWithConversationLock("parent") {})
    }

    private fun family(parentId: String, children: List<String>) {
        coEvery { dao.getSubagentFamilyRootId(parentId) } returns parentId
        coEvery { dao.getSubagentIds(parentId) } returns children
        children.forEach { id ->
            coEvery { dao.getSubagentFamilyRootId(id) } returns parentId
        }
        coEvery { dao.recoverConversationRuntime(any(), any()) } returns 0
    }
}
