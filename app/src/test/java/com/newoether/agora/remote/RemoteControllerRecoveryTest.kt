package com.newoether.agora.remote

import io.mockk.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.launch
import kotlinx.coroutines.flow.MutableSharedFlow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.*
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
internal class RemoteControllerRecoveryTest : RemoteViewModelFixture() {
    private fun TestScope.open(events: MutableSharedFlow<RemoteConversationPage>? = null): RemoteViewModel {
        if (events != null) every { client.events(any()) } returns events
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = kotlinx.coroutines.CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent(); saveAndSelect(vm); vm.setVisible(true); runCurrent()
        vm.selectSession(session); runCurrent()
        return vm
    }

    @Test fun offPageReceiptConfirmsExactInputOnceWithoutFetchingHistoryOrErasingEditedText() = runTest(dispatcher) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        coEvery { client.send(any(), any(), any(), any()) } returns "turn"
        val vm = open(events)
        val owner = vm.state.value.owner!!
        vm.editDraft(owner, "sent"); vm.send(); runCurrent()
        val attempt = vm.state.value.attempts[owner]!!
        assertEquals(RemoteDelivery.ACCEPTED, attempt.delivery)
        assertEquals("turn", attempt.turnId)
        vm.editDraft(owner, "new draft")
        val tail = RemoteMessage("answer", "turn", null, "assistant", "long answer tail", 1)
        val page = bodyPage(listOf(tail), "older", emptyList(), RemoteRuntime("idle", completedTurnId = "turn"))
        events.emit(page.copy(inputReceipts = listOf(RemoteInputReceipt(attempt.clientId, "wrong-turn", "user"))))
        runCurrent()
        assertEquals(RemoteDelivery.ACCEPTED, vm.state.value.attempts[owner]?.delivery)
        val confirmed = page.copy(inputReceipts = listOf(RemoteInputReceipt(attempt.clientId, "turn", "user")))
        events.emit(confirmed); runCurrent()
        assertEquals(RemoteDelivery.DELIVERED, vm.state.value.attempts[owner]?.delivery)
        assertEquals("new draft", vm.state.value.drafts[owner])
        val scroll = vm.animatedScrollRequest.value!!
        assertEquals("answer", scroll.targetMessageId)
        vm.completeAnimatedScroll(scroll.id)
        events.emit(confirmed); runCurrent()
        assertNull(vm.animatedScrollRequest.value)
        coVerify(exactly = 0) { client.conversation(any(), "older") }
        coVerify(exactly = 1) { client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun loadedUserProofMustMatchTheAcceptedTurn() = runTest(dispatcher) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        coEvery { client.send(any(), any(), any(), any()) } returns "accepted-turn"
        val vm = open(events)
        val owner = vm.state.value.owner!!
        vm.editDraft(owner, "sent"); vm.send(); runCurrent()
        val id = vm.state.value.attempts[owner]!!.clientId
        val wrong = RemoteMessage("wrong-user", "another-turn", id, "user", "sent", 0)
        events.emit(bodyPage(listOf(wrong), null, emptyList(), RemoteRuntime("idle"))); runCurrent()
        assertEquals(RemoteDelivery.ACCEPTED, vm.state.value.attempts[owner]?.delivery)
        val right = wrong.copy(id = "right-user", turnId = "accepted-turn")
        events.emit(bodyPage(listOf(wrong, right), null, emptyList(), RemoteRuntime("idle"))); runCurrent()
        assertEquals(RemoteDelivery.DELIVERED, vm.state.value.attempts[owner]?.delivery)
        vm.setVisible(false)
    }

    @Test fun offPageNativeInputCanConfirmBeforeALostHttpReceipt() = runTest(dispatcher) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        val receipt = CompletableDeferred<String>()
        coEvery { client.send(any(), any(), any(), any()) } coAnswers { receipt.await() }
        val vm = open(events)
        val owner = vm.state.value.owner!!
        vm.editDraft(owner, "sent"); vm.send(); runCurrent()
        val id = vm.state.value.attempts[owner]!!.clientId
        events.emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("idle"))
            .copy(inputReceipts = listOf(RemoteInputReceipt(id, "turn", "user"))))
        runCurrent()
        assertEquals(RemoteDelivery.DELIVERED, vm.state.value.attempts[owner]?.delivery)
        assertNull(vm.state.value.drafts[owner])
        receipt.completeExceptionally(IOException("Lost receipt")); runCurrent()
        assertEquals(RemoteDelivery.DELIVERED, vm.state.value.attempts[owner]?.delivery)
        coVerify(exactly = 1) { client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun explicitOlderPageReconcilesLegacyOffPageClientConfirmation() = runTest(dispatcher) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        coEvery { client.send(any(), any(), any(), any()) } returns "turn"
        val vm = open(events)
        val owner = vm.state.value.owner!!
        vm.editDraft(owner, "sent"); vm.send(); runCurrent()
        val id = vm.state.value.attempts[owner]!!.clientId
        events.emit(bodyPage(listOf(RemoteMessage("answer", "turn", null, "assistant", "tail", 1)),
            "older", emptyList(), RemoteRuntime("idle")))
        vm.state.first { !it.loading }
        runCurrent()
        assertNull(vm.state.value.failure)
        assertEquals("older", vm.state.value.historyCursor)
        assertFalse(vm.state.value.loading)
        coEvery { client.conversation("session", "older") } returns bodyPage(
            listOf(RemoteMessage("user", "turn", id, "user", "sent", 0)), null, emptyList())
            .copy(pageCursor = "older")
        vm.loadMore(); runCurrent()
        assertNull(vm.state.value.failure)
        assertEquals(listOf("user", "answer"), vm.state.value.nodes.map { it.id })
        assertEquals(RemoteDelivery.DELIVERED, vm.state.value.attempts[owner]?.delivery)
        assertNull(vm.state.value.drafts[owner])
        coVerify(exactly = 1) { client.conversation("session", "older") }
        vm.setVisible(false)
    }

    @Test fun unknownStatusCannotEndStopButExactTerminalEvidenceCan() = runTest(dispatcher) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        coEvery { client.stop(any(), any()) } returns Unit
        val vm = open(events)
        events.emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("active", "turn"))); runCurrent()
        vm.stop(); runCurrent()
        events.emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("notLoaded"))); runCurrent()
        assertTrue(vm.state.value.isStopping)
        events.emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("notLoaded", completedTurnId = "turn")))
        runCurrent()
        assertFalse(vm.state.value.isStopping)
        vm.setVisible(false)
    }

    @Test fun stopNativeBarrierSurvivesDisconnectBeforeTheHttpBarrier() = runTest(dispatcher) {
        val disconnect = CompletableDeferred<Unit>()
        val terminal = CompletableDeferred<Unit>()
        val receipt = CompletableDeferred<Unit>()
        every { client.events(any()) } returns flow {
            emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("active", "turn")))
            terminal.await()
            emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("idle", completedTurnId = "turn")))
            disconnect.await()
            throw IOException("Disconnected after native completion")
        }
        coEvery { client.conversation(any(), any()) } throws IOException("No fallback snapshot")
        coEvery { client.stop(any(), any()) } coAnswers { receipt.await() }
        val vm = open()
        vm.stop(); runCurrent()
        terminal.complete(Unit); runCurrent()
        assertTrue(vm.state.value.stoppingTurnEnded)
        disconnect.complete(Unit); runCurrent()
        assertNull(vm.state.value.runtime)
        assertTrue(vm.state.value.isStopping)
        receipt.complete(Unit); runCurrent()
        assertFalse(vm.state.value.isStopping)
        coVerify(exactly = 1) { client.stop("session", "turn") }
        vm.setVisible(false)
    }

    @Test fun oldStopCompletionCannotClearANewStopForTheSameTurnAfterNavigation() = runTest(dispatcher) {
        verifyReopenedStop(oldFails = false)
    }

    @Test fun oldStopFailureCannotClearANewStopForTheSameTurnAfterNavigation() = runTest(dispatcher) {
        verifyReopenedStop(oldFails = true)
    }

    private suspend fun TestScope.verifyReopenedStop(oldFails: Boolean) {
        val events = MutableSharedFlow<RemoteConversationPage>()
        val oldReceipt = CompletableDeferred<Unit>()
        val newReceipt = CompletableDeferred<Unit>()
        var stopCalls = 0
        coEvery { client.stop(any(), any()) } coAnswers {
            if (stopCalls++ == 0) oldReceipt.await() else newReceipt.await()
        }
        val vm = open(events)
        val active = bodyPage(emptyList(), null, emptyList(), RemoteRuntime("active", "turn"))
        events.emit(active); runCurrent()
        vm.stop(); runCurrent()
        vm.selectSession(null); runCurrent()
        vm.selectSession(session); runCurrent()
        events.emit(active); runCurrent()
        vm.stop(); runCurrent()
        assertEquals(2, stopCalls)
        assertTrue(vm.state.value.controlling)
        assertTrue(vm.state.value.isStopping)
        if (oldFails) oldReceipt.completeExceptionally(IOException("Old Stop failed")) else oldReceipt.complete(Unit)
        runCurrent()
        assertTrue(vm.state.value.controlling)
        assertTrue(vm.state.value.isStopping)
        assertNull(vm.state.value.failure)
        newReceipt.complete(Unit); runCurrent()
        assertFalse(vm.state.value.controlling)
        assertTrue(vm.state.value.isStopping)
        events.emit(bodyPage(emptyList(), null, emptyList(), RemoteRuntime("idle", completedTurnId = "turn")))
        runCurrent()
        assertFalse(vm.state.value.isStopping)
        coVerify(exactly = 2) { client.stop("session", "turn") }
        vm.setVisible(false)
    }

    @Test fun unknownListedStatePreservesActivityUntilTheSameTurnIsConfirmedComplete() {
        val old = RemoteSessionStatus("session", "active", "turn")
        assertEquals(old, reconcileRemoteSessionStatus(old, RemoteSessionStatus("session")))
        assertEquals(old, reconcileRemoteSessionStatus(old, RemoteSessionStatus("session", completedTurnId = "older")))
        val settled = reconcileRemoteSessionStatus(old, RemoteSessionStatus("session", completedTurnId = "turn"))
        assertEquals("idle", settled.status)
        assertNull(settled.activeTurnId)
        assertEquals("turn", settled.completedTurnId)
        assertTrue(settled.hasUnreadTurn)
    }
}
