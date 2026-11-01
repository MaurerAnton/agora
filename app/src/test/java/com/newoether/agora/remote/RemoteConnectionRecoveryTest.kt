package com.newoether.agora.remote

import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.TestScope
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.test.setMain
import kotlinx.coroutines.test.resetMain
import kotlinx.coroutines.withContext
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
class RemoteConnectionRecoveryTest {
    private val dispatcher = StandardTestDispatcher()
    private val client = mockk<FiloClient>()
    private val connections = mockk<RemoteConnectionStore>(relaxed = true)
    private val session = RemoteSession("session", "Existing", "/workspace", 1)
    private val page = bodyPage(emptyList(), null, emptyList(), RemoteRuntime("idle", model = "model"))

    @Before fun setup() {
        Dispatchers.setMain(dispatcher)
        coEvery { connections.load() } returns emptyList()
        every { client.adapters } returns null
        every { client.address } returns "http://computer/"
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        coEvery { client.sessions(any()) } returns RemoteSessionPage(listOf(session), null)
        coEvery { client.models() } returns listOf(RemoteModel("model", "Model", true))
        coEvery { client.conversation(any(), any()) } throws FiloHttpException(503)
    }
    @After fun tearDown() { Dispatchers.resetMain() }

    private fun TestScope.open(): RemoteViewModel {
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = kotlinx.coroutines.CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        vm.saveDevice("http://computer/", "token"); runCurrent()
        vm.selectDevice("http://computer/")
        vm.selectSession(session)
        vm.setVisible(true); runCurrent()
        return vm
    }

    private fun TestScope.notices(vm: RemoteViewModel): MutableList<RemoteNotice> {
        val result = mutableListOf<RemoteNotice>()
        backgroundScope.launch(dispatcher) { vm.notices.collect { result += it } }
        runCurrent()
        return result
    }

    @Test fun sendFailureNoticeKeepsItsOriginalSelectionAndCannotReplayInput() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { emit(page); awaitCancellation() }
        for (navigate in listOf(false, true)) {
            val response = CompletableDeferred<String>()
            coEvery { client.send(any(), any(), any(), any()) } coAnswers { response.await() }
            val vm = open()
            val notices = notices(vm)
            val owner = requireNotNull(vm.state.value.owner)
            vm.editDraft(owner, "Keep this input")
            vm.send(); runCurrent()
            if (navigate) { vm.selectSession(session.copy(id = "other")); runCurrent() }
            response.completeExceptionally(IOException("Lost response")); runCurrent()
            val notice = notices.single()
            assertEquals("send_failed", notice.stage)
            assertEquals("delivery_unconfirmed", notice.code)
            assertNull(notice.detail)
            assertFalse(notice.canRetryRead)
            assertEquals(!navigate, vm.isNoticeCurrent(notice))
            assertEquals(RemoteDelivery.UNKNOWN, vm.state.value.attempts[owner]?.delivery)
            assertEquals("Keep this input", vm.state.value.drafts[owner])
            vm.retryNotice(notice); runCurrent()
            if (navigate) {
                vm.selectSession(session); runCurrent()
                assertFalse(vm.isNoticeCurrent(notice))
            }
            vm.setVisible(false)
        }
        coVerify(exactly = 2) { client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
    }

    @Test fun transientStreamDisconnectRecoversWithoutOfflineOrSnackbar() = runTest(dispatcher) {
        var reads = 0
        every { client.events(any()) } answers { flow {
            if (++reads == 1) throw IOException("connection reset")
            emit(page); awaitCancellation()
        } }
        val vm = open()
        val notices = notices(vm)
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertNull(vm.state.value.failure)
        assertNull(vm.state.value.runtime)
        assertTrue(notices.isEmpty())
        advanceTimeBy(3000); runCurrent()
        assertEquals(2, reads)
        assertEquals("idle", vm.state.value.runtime?.status)
        assertTrue(notices.isEmpty())
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun persistentSessionReadFailureKeepsHealthyDeviceOnlineAndReportsOnce() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw IOException("stream failed") }
        val vm = open()
        val notices = notices(vm)
        repeat(3) { advanceTimeBy(3000); runCurrent() }
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.SERVICE, vm.state.value.failure)
        assertEquals(listOf(RemoteFailure.SERVICE), notices.map { it.failure })
        assertNull(vm.state.value.runtime)
        vm.setVisible(false)
    }

    @Test fun nativeHttpFailureDoesNotTurnAReachableDeviceOffline() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw FiloHttpException(503) }
        val vm = open()
        val notices = notices(vm)
        repeat(2) { advanceTimeBy(3000); runCurrent() }
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(listOf(RemoteFailure.SERVICE), notices.map { it.failure })
        coVerify(exactly = 3) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun desktopUnavailableReadImmediatelyUpdatesNativeStatusAndSafeRetryPreservesHistoryAndDraft() = runTest(dispatcher) {
        val disconnect = CompletableDeferred<Unit>()
        // Native user history avoids Markdown's independent dispatcher in the timed assertions.
        val message = RemoteMessage("input", "turn", null, "user", "Retained native input", 1)
        val history = bodyPage(listOf(message), null, emptyList(), page.runtime)
        var reads = 0
        every { client.events(any()) } answers { flow {
            emit(history)
            if (++reads == 1) {
                disconnect.await()
                throw FiloHttpException(502, code = "desktop_unavailable")
            }
            awaitCancellation()
        } }
        val vm = open()
        assertEquals(listOf(message.id), vm.state.value.nodes.map { it.id })
        val notices = notices(vm)
        val owner = requireNotNull(vm.state.value.owner)
        vm.editDraft(owner, "Keep the unsent draft")
        val loaded = vm.state.value
        assertEquals(RemoteDeviceStatus.CONNECTED, loaded.devices.single().desktopStatus)
        disconnect.complete(Unit)
        runCurrent()
        val failed = vm.state.value
        assertEquals(RemoteDeviceStatus.CONNECTED, failed.devices.single().status)
        assertEquals(RemoteDeviceStatus.ERROR, failed.devices.single().desktopStatus)
        assertNull(failed.devices.single().failure)
        assertEquals(RemoteFailure.SERVICE, failed.failure)
        assertEquals(loaded.nodes, failed.nodes)
        assertEquals(loaded.drafts, failed.drafts)
        assertEquals(loaded.owner, failed.owner)
        assertFalse(failed.loading)
        val notice = notices.single()
        assertEquals("read_failed", notice.stage)
        assertEquals("desktop_unavailable", notice.code)
        assertTrue(notice.canRetryRead)
        assertTrue(vm.isNoticeCurrent(notice))
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { client.conversation(session.id, null) }
        vm.retryNotice(notice)
        runCurrent()
        assertEquals(2, reads)
        assertNull(vm.state.value.failure)
        assertEquals(loaded.nodes, vm.state.value.nodes)
        assertEquals(loaded.drafts, vm.state.value.drafts)
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().desktopStatus)
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().desktopStatus)
        assertEquals(loaded.drafts, vm.state.value.drafts)
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.updateSettings(any(), any()); client.setModel(any(), any()); client.stop(any(), any()) }
        vm.setVisible(false)
    }

    @Test fun protocolFailurePreservesDeviceHealthAndUsesProtocolNotice() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw IllegalArgumentException("invalid page") }
        val vm = open()
        val notices = notices(vm)
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.PROTOCOL, notices.single().failure)
        assertNull(vm.state.value.runtime)
        vm.setVisible(false)
    }

    @Test fun trueOutageReportsOfflineThenShowsConnectingUntilNewSnapshot() = runTest(dispatcher) {
        val disconnect = CompletableDeferred<Unit>()
        val reconnected = CompletableDeferred<Unit>()
        var reads = 0
        every { client.events(any()) } answers { flow {
            if (++reads == 1) {
                emit(page); disconnect.await(); throw IOException("offline")
            }
            reconnected.await(); emit(page); awaitCancellation()
        } }
        val vm = open()
        val notices = notices(vm)
        coEvery { client.connect() } throws IOException("offline")
        disconnect.complete(Unit); runCurrent()
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.NETWORK, notices.single().failure)
        assertNull(vm.state.value.runtime)
        advanceTimeBy(3000); runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().status)
        reconnected.complete(Unit); runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertNull(vm.state.value.failure)
        assertEquals("idle", vm.state.value.runtime?.status)
        vm.setVisible(false)
    }

    @Test fun failedHealthCheckIsNotHiddenByTransientStreamGrace() = runTest(dispatcher) {
        val disconnect = CompletableDeferred<Unit>()
        every { client.events(any()) } returns flow { disconnect.await(); throw IOException("reset") }
        val vm = open()
        val notices = notices(vm)
        coEvery { client.connect() } throws FiloHttpException(503)
        disconnect.complete(Unit); runCurrent()
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.SERVICE, notices.single().failure)
        vm.setVisible(false)
    }

    @Test fun authenticationFailureRemainsImmediateAndCannotEnableControls() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw FiloHttpException(401) }
        val vm = open()
        val notices = notices(vm)
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.AUTHENTICATION, notices.single().failure)
        assertFalse(vm.state.value.canEditSettings)
        vm.setVisible(false)
    }

    @Test fun staleHealthResultCannotOverwriteNewSelectionOrStartAnotherRead() = runTest(dispatcher) {
        val disconnect = CompletableDeferred<Unit>()
        val health = CompletableDeferred<RemoteDeviceStatus>()
        every { client.events(any()) } returns flow { disconnect.await(); throw IOException("reset") }
        val vm = open()
        val notices = notices(vm)
        coEvery { client.connect() } coAnswers { withContext(NonCancellable) { health.await() } }
        disconnect.complete(Unit); runCurrent()
        vm.setVisible(false)
        vm.selectSession(null)
        health.completeExceptionally(IOException("old read failed")); runCurrent()
        assertNull(vm.state.value.session)
        assertNull(vm.state.value.failure)
        assertTrue(notices.isEmpty())
        advanceTimeBy(6000); runCurrent()
        io.mockk.verify(exactly = 1) { client.events(any()) }
    }

    @Test fun foregroundRetryStaysLoadingThroughFallbackAndHealthThenBackgroundFailuresStaySettled() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw IOException("offline") }
        coEvery { client.connect() } throws IOException("offline")
        val vm = open()
        val failed = vm.state.value
        assertEquals(RemoteFailure.NETWORK, failed.failure)
        assertFalse(failed.loading)
        val owner = requireNotNull(failed.owner)
        vm.editDraft(owner, "Keep this draft")
        val health = CompletableDeferred<RemoteDeviceStatus>()
        val fallback = CompletableDeferred<RemoteConversationPage>()
        var probes = 0
        coEvery { client.connect() } coAnswers {
            if (++probes == 1) RemoteDeviceStatus.CONNECTED else health.await()
        }
        coEvery { client.conversation(session.id, null) } coAnswers { fallback.await() }
        vm.retryConnection()
        vm.retryConnection()
        runCurrent()
        assertTrue(vm.state.value.loading)
        assertEquals(failed.failure, vm.state.value.failure)
        assertEquals(1, probes)
        fallback.completeExceptionally(IOException("snapshot offline"))
        runCurrent()
        assertTrue(vm.state.value.loading)
        assertEquals(failed.failure, vm.state.value.failure)
        assertEquals(2, probes)
        health.completeExceptionally(IOException("health offline"))
        runCurrent()
        assertFalse(vm.state.value.loading)
        assertEquals(RemoteFailure.NETWORK, vm.state.value.failure)
        assertEquals("Keep this draft", vm.state.value.drafts[owner])
        coEvery { client.conversation(session.id, null) } throws IOException("snapshot offline")
        repeat(3) {
            advanceTimeBy(3000); runCurrent()
            assertFalse(vm.state.value.loading)
            assertEquals(RemoteFailure.NETWORK, vm.state.value.failure)
        }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun hidingDuringForegroundFallbackFencesItsLateFailureAndClearsLoading() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw FiloHttpException(503) }
        val vm = open()
        val pending = CompletableDeferred<RemoteConversationPage>()
        coEvery { client.conversation(session.id, null) } coAnswers { withContext(NonCancellable) { pending.await() } }
        vm.retryConnection(); runCurrent()
        assertTrue(vm.state.value.loading)
        vm.setVisible(false)
        vm.selectSession(null)
        assertFalse(vm.state.value.loading)
        pending.completeExceptionally(IOException("late failure")); runCurrent()
        assertNull(vm.state.value.session)
        assertNull(vm.state.value.failure)
        assertFalse(vm.state.value.loading)
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
    }

    @Test fun successfulSnapshotEndsFailureEpisodeAndAllowsANewNotice() = runTest(dispatcher) {
        val failAgain = CompletableDeferred<Unit>()
        var reads = 0
        every { client.events(any()) } answers { flow {
            if (++reads == 2) { emit(page); failAgain.await() }
            throw FiloHttpException(503)
        } }
        val vm = open()
        val notices = notices(vm)
        assertEquals(1, notices.size)
        advanceTimeBy(3000); runCurrent()
        assertNull(vm.state.value.failure)
        failAgain.complete(Unit); runCurrent()
        assertEquals(2, notices.size)
        vm.setVisible(false)
    }

    @Test fun historyIsReadableWhenStreamFailsButSameOwnersSnapshotIsAvailable() = runTest(dispatcher) {
        every { client.events(any()) } returns flow { throw IOException("stream unavailable") }
        // Native input keeps this fallback status test on the injected dispatcher.
        val message = RemoteMessage("input", "turn", null, "user", "Retained native input", 1)
        coEvery { client.conversation("session", null) } returns bodyPage(listOf(message), null, emptyList(), page.runtime)
        val vm = open()
        val notices = notices(vm)
        assertEquals(listOf(message.id), vm.state.value.nodes.map { it.id })
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertFalse(vm.state.value.loading)
        assertNull(vm.state.value.runtime)
        assertEquals("model", vm.state.value.selectedModel)
        assertFalse(vm.state.value.canEditSettings)
        assertTrue(notices.isEmpty())
        advanceTimeBy(3000); runCurrent()
        assertEquals(listOf(message.id), vm.state.value.nodes.map { it.id })
        assertEquals(RemoteFailure.SERVICE, notices.single().failure)
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }
}
