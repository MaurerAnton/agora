package com.newoether.agora.remote

import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withContext
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
internal class RemoteDesktopEvidenceTest : RemoteViewModelFixture() {
    private fun TestScope.open(draft: Boolean): RemoteViewModel {
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        saveAndSelect(vm)
        vm.setVisible(true)
        runCurrent()
        if (draft) vm.newSession() else vm.selectSession(session)
        runCurrent()
        return vm
    }

    private fun assertNoNativeMutations() {
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.updateSettings(any(), any()) }
        coVerify(exactly = 0) { client.setModel(any(), any()) }
        coVerify(exactly = 0) { client.stop(any(), any()) }
        coVerify(exactly = 0) { client.upload(any()) }
    }

    private fun TestScope.nativeUnavailableSupersedesOldInfo(staleFails: Boolean) {
        val unavailable = CompletableDeferred<Unit>(backgroundScope.coroutineContext[Job])
        val fallback = CompletableDeferred<Result<RemoteConversationPage>>(backgroundScope.coroutineContext[Job])
        val stale = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
        // Native user history keeps this status timeline on the injected test dispatcher.
        val message = RemoteMessage("input", "turn", null, "user", "Existing native input", 1)
        val history = bodyPage(listOf(message), null, emptyList(), RemoteRuntime("idle", model = "model"))
        every { client.events(session.id) } returns flow {
            emit(history)
            unavailable.await()
            throw FiloHttpException(502, code = "desktop_unavailable")
        }
        var fallbackEntered = false
        coEvery { client.conversation(session.id, null) } coAnswers {
            fallbackEntered = true
            fallback.await().getOrThrow()
        }
        val vm = open(draft = false)
        val owner = requireNotNull(vm.state.value.owner)
        vm.editDraft(owner, "Preserve this unsent input")
        val loaded = vm.state.value
        assertEquals(listOf(message.id), loaded.nodes.map { it.id })
        assertEquals(RemoteDeviceStatus.CONNECTED, loaded.devices.single().desktopStatus)
        var staleResumed = false
        coEvery { client.connect() } coAnswers {
            withContext(NonCancellable) {
                val result = stale.await()
                staleResumed = true
                result.getOrThrow()
            }
        }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        assertFalse(fallbackEntered)
        // The new native evidence arrives after the old info request has already started.
        advanceTimeBy(1000)
        runCurrent()
        unavailable.complete(Unit)
        runCurrent()
        assertTrue("The fallback read is still waiting", fallbackEntered && !fallback.isCompleted)
        val failed = vm.state.value
        assertEquals(RemoteDeviceStatus.ERROR, failed.devices.single().desktopStatus)
        assertEquals(RemoteDeviceStatus.CONNECTED, failed.devices.single().status)
        assertNull(failed.devices.single().failure)
        assertEquals(loaded.nodes, failed.nodes)
        assertEquals(loaded.messageGroups, failed.messageGroups)
        assertEquals(loaded.drafts, failed.drafts)
        assertEquals(loaded.owner, failed.owner)
        assertNull(failed.runtime)
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        stale.complete(if (staleFails) Result.failure(IOException("Old info failed"))
            else Result.success(RemoteDeviceStatus.CONNECTED))
        runCurrent()
        assertTrue("The old response must be delivered before checking its rejection", staleResumed)
        assertEquals(failed, vm.state.value)
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        assertEquals(failed, vm.state.value)
        advanceTimeBy(1)
        runCurrent()
        assertEquals(failed.copy(devices = failed.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
        }), vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        assertFalse("Recovery cannot depend on the fallback completing", fallback.isCompleted)
        // Finishing the older failure must not reapply its native error after fresh info recovered.
        fallback.complete(Result.failure(FiloHttpException(503)))
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().desktopStatus)
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.SERVICE, vm.state.value.failure)
        assertEquals(loaded.nodes, vm.state.value.nodes)
        assertEquals(loaded.drafts, vm.state.value.drafts)
        coVerify(exactly = 1) { client.conversation(session.id, null) }
        coVerify(exactly = 1) { client.sessions(any()) }
        assertNoNativeMutations()
        vm.setVisible(false)
    }

    @Test fun nativeUnavailableBeforePendingFallbackRejectsOlderConnectedInfo() = runTest(dispatcher) {
        nativeUnavailableSupersedesOldInfo(staleFails = false)
    }

    @Test fun nativeUnavailableBeforePendingFallbackRejectsOlderFailedInfo() = runTest(dispatcher) {
        nativeUnavailableSupersedesOldInfo(staleFails = true)
    }

    private fun TestScope.explicitDraftCheckSupersedesOldInfo(checkFails: Boolean, staleFails: Boolean) {
        val vm = open(draft = true)
        vm.editDraft(requireNotNull(vm.state.value.owner), "Keep the local draft")
        val draft = vm.state.value
        val stale = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
        val check = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
        var staleResumed = false
        coEvery { client.connect() } coAnswers {
            withContext(NonCancellable) {
                val result = stale.await()
                staleResumed = true
                result.getOrThrow()
            }
        }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        coEvery { client.connect() } coAnswers { check.await().getOrThrow() }
        vm.retryConnection()
        vm.retryConnection()
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, vm.state.value.devices.single().desktopStatus)
        coVerify(exactly = 3) { client.connect() }
        check.complete(if (checkFails) Result.failure(FiloHttpException(401))
            else Result.success(RemoteDeviceStatus.IDLE))
        runCurrent()
        val checked = vm.state.value
        assertEquals(if (checkFails) RemoteDeviceStatus.ERROR else RemoteDeviceStatus.CONNECTED,
            checked.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, checked.devices.single().desktopStatus)
        assertEquals(if (checkFails) RemoteFailure.AUTHENTICATION else null, checked.devices.single().failure)
        assertEquals(draft.owner, checked.owner)
        assertEquals(draft.session, checked.session)
        assertEquals(draft.drafts, checked.drafts)
        assertEquals(draft.attachments, checked.attachments)
        assertEquals(draft.models, checked.models)
        assertEquals(draft.draftSettings, checked.draftSettings)
        stale.complete(if (staleFails) Result.failure(IOException("Stale observer failed"))
            else Result.success(RemoteDeviceStatus.CONNECTED))
        runCurrent()
        assertTrue("The cancelled observation must actually finish", staleResumed)
        assertEquals(checked, vm.state.value)
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        if (checkFails) {
            advanceTimeBy(9000)
            runCurrent()
            coVerify(exactly = 3) { client.connect() }
            assertEquals(checked, vm.state.value)
        } else {
            advanceTimeBy(2999)
            runCurrent()
            coVerify(exactly = 3) { client.connect() }
            advanceTimeBy(1)
            runCurrent()
            coVerify(exactly = 4) { client.connect() }
            assertEquals(checked.copy(devices = checked.devices.map {
                it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
            }), vm.state.value)
        }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.models() }
        coVerify(exactly = 0) { client.conversation(any(), any()) }
        assertNoNativeMutations()
        vm.setVisible(false)
    }

    @Test fun lateSnapshotAfterGatewayOutageRestartsDesktopObservationWithAFullInterval() = runTest(dispatcher) {
        val disconnected = CompletableDeferred<Unit>(backgroundScope.coroutineContext[Job])
        val recovered = CompletableDeferred<Unit>(backgroundScope.coroutineContext[Job])
        val message = RemoteMessage("input", "turn", null, "user", "Retained through outage", 1)
        val history = bodyPage(listOf(message), null, emptyList(), RemoteRuntime("idle", model = "model"))
        var reads = 0
        every { client.events(session.id) } answers { flow {
            if (++reads == 1) {
                emit(history)
                disconnected.await()
                throw IOException("Gateway disconnected")
            }
            recovered.await()
            emit(history)
            awaitCancellation()
        } }
        coEvery { client.conversation(session.id, null) } throws IOException("Snapshot unavailable during outage")
        val vm = open(draft = false)
        vm.editDraft(requireNotNull(vm.state.value.owner), "Keep this draft through recovery")
        val loaded = vm.state.value
        assertEquals(listOf(message.id), loaded.nodes.map { it.id })
        advanceTimeBy(1000)
        runCurrent()
        coEvery { client.connect() } throws IOException("Gateway offline")
        disconnected.complete(Unit)
        runCurrent()
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, vm.state.value.devices.single().desktopStatus)
        assertEquals(RemoteFailure.NETWORK, vm.state.value.failure)
        assertEquals(loaded.nodes, vm.state.value.nodes)
        assertEquals(loaded.drafts, vm.state.value.drafts)
        coVerify(exactly = 2) { client.connect() }
        // The original observer reaches its next tick and exits before the second live read recovers.
        advanceTimeBy(6000)
        runCurrent()
        assertEquals(2, reads)
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().status)
        coVerify(exactly = 2) { client.connect() }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        recovered.complete(Unit)
        runCurrent()
        val restored = vm.state.value
        assertEquals(RemoteDeviceStatus.CONNECTED, restored.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, restored.devices.single().desktopStatus)
        assertNull(restored.failure)
        assertEquals("idle", restored.runtime?.status)
        assertEquals(loaded.nodes, restored.nodes)
        assertEquals(loaded.owner, restored.owner)
        assertEquals(loaded.drafts, restored.drafts)
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(2999)
        runCurrent()
        assertEquals(restored, vm.state.value)
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(restored.copy(devices = restored.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
        }), vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(restored.copy(devices = restored.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTING)
        }), vm.state.value)
        coVerify(exactly = 4) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.conversation(session.id, null) }
        assertNoNativeMutations()
        vm.setVisible(false)
    }

    @Test fun successfulDraftCheckRejectsOlderConnectedInfo() = runTest(dispatcher) {
        explicitDraftCheckSupersedesOldInfo(checkFails = false, staleFails = false)
    }

    @Test fun successfulDraftCheckRejectsOlderFailedInfo() = runTest(dispatcher) {
        explicitDraftCheckSupersedesOldInfo(checkFails = false, staleFails = true)
    }

    @Test fun failedDraftCheckRejectsOlderConnectedInfoWithoutRestartingObservation() = runTest(dispatcher) {
        explicitDraftCheckSupersedesOldInfo(checkFails = true, staleFails = false)
    }

    @Test fun failedDraftCheckRejectsOlderFailedInfoWithoutRestartingObservation() = runTest(dispatcher) {
        explicitDraftCheckSupersedesOldInfo(checkFails = true, staleFails = true)
    }
}
