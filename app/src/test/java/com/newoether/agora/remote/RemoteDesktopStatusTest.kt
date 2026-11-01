package com.newoether.agora.remote

import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.CoroutineStart
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.Job
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.awaitCancellation
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
internal class RemoteDesktopStatusTest : RemoteViewModelFixture() {
    private val address = "http://computer/"

    private fun TestScope.openCatalog(replacement: FiloClient? = null): RemoteViewModel {
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, token ->
            if (token == "replacement") requireNotNull(replacement) else client
        }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        saveAndSelect(vm)
        vm.setVisible(true)
        runCurrent()
        return vm
    }

    @Test fun successfulGatewayAdmissionKeepsEveryDesktopStateIndependent() = runTest(dispatcher) {
        for (desktop in RemoteDeviceStatus.entries) {
            coEvery { client.connect() } returns desktop
            val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
            runCurrent()
            vm.saveDevice(address, "token", "Configured device")
            runCurrent()
            val device = vm.state.value.devices.single()
            assertEquals(RemoteDeviceStatus.CONNECTED, device.status)
            assertEquals(desktop, device.desktopStatus)
            assertNull(device.failure)
            assertEquals("Configured device", device.name)
        }
        coVerify(exactly = 0) { client.sessions(any()) }
    }

    @Test fun emptyCatalogObservesDesktopAfterThreeSecondsWithoutVisibleRowsOrExtraEntryRequest() = runTest(dispatcher) {
        coEvery { client.connect() } returns RemoteDeviceStatus.IDLE
        coEvery { client.sessions(any()) } returns RemoteSessionPage(emptyList(), null)
        val vm = openCatalog()
        vm.observeSessions(address, emptyList())
        runCurrent()
        val admitted = vm.state.value
        assertTrue(admitted.sessions.isEmpty())
        assertFalse(admitted.loading)
        assertEquals(RemoteDeviceStatus.IDLE, admitted.devices.single().desktopStatus)
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(admitted.copy(devices = admitted.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
        }), vm.state.value)
        for (desktop in listOf(RemoteDeviceStatus.CONNECTING, RemoteDeviceStatus.ERROR,
                RemoteDeviceStatus.IDLE, RemoteDeviceStatus.CONNECTED)) {
            coEvery { client.connect() } returns desktop
            val before = vm.state.value
            advanceTimeBy(3000)
            runCurrent()
            assertEquals(before.copy(devices = before.devices.map {
                it.copy(desktopStatus = desktop)
            }), vm.state.value)
        }
        coVerify(exactly = 6) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 0) { client.conversation(any(), any()) }
        vm.setVisible(false)
    }

    @Test fun failedObservationsClearOnlyDesktopAndPreserveCatalogHealthAndUnreadState() = runTest(dispatcher) {
        val status = RemoteSessionStatus(session.id, "active", activeTurnId = "active-turn",
            completedTurnId = "completed-turn", hasUnreadTurn = true)
        coEvery { client.sessions(any()) } returns RemoteSessionPage(listOf(session), null, listOf(status))
        val vm = openCatalog()
        val notices = mutableListOf<RemoteNotice>()
        backgroundScope.launch(dispatcher) { vm.notices.collect { notices += it } }
        runCurrent()
        val admitted = vm.state.value
        assertTrue(admitted.hasUnreadGeneration(session.id))
        for (error in listOf(IOException("offline"), FiloHttpException(401), FiloHttpException(503),
                IllegalArgumentException("invalid info"))) {
            coEvery { client.connect() } throws error
            advanceTimeBy(3000)
            runCurrent()
            assertEquals(admitted.copy(devices = admitted.devices.map {
                it.copy(desktopStatus = RemoteDeviceStatus.IDLE)
            }), vm.state.value)
            assertTrue(notices.isEmpty())
        }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(admitted, vm.state.value)
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 0) { connections.markViewed(any(), any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun changingVisibleSessionRowsDoesNotRestartOrDriveDesktopObservation() = runTest(dispatcher) {
        val firstStatus = RemoteSessionStatus(session.id, "active", activeTurnId = "native-turn")
        coEvery { client.sessions(any()) } returns RemoteSessionPage(listOf(session), null, listOf(firstStatus))
        val vm = openCatalog()
        vm.observeSessions(address, listOf(session.id))
        advanceTimeBy(1000)
        runCurrent()
        vm.observeSessions(address, emptyList())
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        advanceTimeBy(2000)
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().desktopStatus)
        assertEquals(firstStatus, vm.state.value.sessionStatuses["$address/${session.id}"])
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        val completed = RemoteSessionStatus(session.id, "idle", completedTurnId = "native-turn", hasUnreadTurn = true)
        coEvery { client.sessions(any()) } returns RemoteSessionPage(listOf(session), null, listOf(completed))
        vm.observeSessions(address, listOf(session.id))
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(completed, vm.state.value.sessionStatuses["$address/${session.id}"])
        assertTrue(vm.state.value.hasUnreadGeneration(session.id))
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().desktopStatus)
        coVerify(exactly = 3) { client.connect() }
        coVerify(exactly = 2) { client.sessions(any()) }
        vm.setVisible(false)
    }

    @Test fun emptyVisibleRowsAfterInitialAuthenticationFailureDoNotStartDesktopObservation() = runTest(dispatcher) {
        coEvery { connections.load() } returns listOf(RemoteConnection("Computer", address, "token"))
        coEvery { client.connect() } throws FiloHttpException(401)
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        vm.selectDevice(address)
        vm.setVisible(true)
        runCurrent()
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.AUTHENTICATION, vm.state.value.devices.single().failure)
        val failed = vm.state.value
        vm.observeSessions(address, emptyList())
        runCurrent()
        advanceTimeBy(9000)
        runCurrent()
        assertEquals(failed, vm.state.value)
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 0) { client.sessions(any()) }
        coVerify(exactly = 0) { client.models() }
        coVerify(exactly = 0) { connections.save(any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun emptyVisibleRowsDuringPendingAuthenticationCannotLeaveAnObserverAfterRejection() = runTest(dispatcher) {
        coEvery { connections.load() } returns listOf(RemoteConnection("Computer", address, "token"))
        val pending = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
        coEvery { client.connect() } coAnswers { pending.await().getOrThrow() }
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        vm.selectDevice(address)
        vm.setVisible(true)
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().status)
        vm.observeSessions(address, emptyList())
        runCurrent()
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        pending.complete(Result.failure(FiloHttpException(401)))
        runCurrent()
        assertEquals(RemoteDeviceStatus.ERROR, vm.state.value.devices.single().status)
        assertEquals(RemoteFailure.AUTHENTICATION, vm.state.value.devices.single().failure)
        val failed = vm.state.value
        // No new visible-row callback: any observer admitted while pending would survive here.
        advanceTimeBy(9000)
        runCurrent()
        assertEquals(failed, vm.state.value)
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 0) { client.sessions(any()) }
        coVerify(exactly = 0) { client.models() }
        coVerify(exactly = 0) { connections.save(any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun lateSaveOnAnotherDevicePreservesTheSelectedCatalogsDesktopObservationAndInterval() = runTest(dispatcher) {
        val otherAddress = "http://second/"
        val other = mockk<FiloClient>()
        val replacement = mockk<FiloClient>()
        every { other.adapters } returns null
        every { other.address } returns otherAddress
        coEvery { other.connect() } returns RemoteDeviceStatus.CONNECTED
        coEvery { other.sessions(any()) } returns RemoteSessionPage(emptyList(), null)
        coEvery { other.models() } returns emptyList()
        every { replacement.adapters } returns null
        every { replacement.address } returns address
        coEvery { replacement.connect() } returns RemoteDeviceStatus.IDLE
        coEvery { connections.load() } returns listOf(
            RemoteConnection("First computer", address, "token"),
            RemoteConnection("Second computer", otherAddress, "token"),
        )
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { url, token ->
            when {
                url == otherAddress -> other
                token == "replacement" -> replacement
                else -> client
            }
        }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        vm.setVisible(true)
        runCurrent()
        val saving = CompletableDeferred<Unit>(backgroundScope.coroutineContext[Job])
        coEvery { connections.save(any(), address) } coAnswers { saving.await() }
        vm.editDevice(address)
        vm.saveDevice(address, "replacement", "Renamed first computer")
        runCurrent()
        assertTrue(vm.state.value.saving)
        vm.selectDevice(otherAddress)
        runCurrent()
        assertTrue(vm.state.value.sessions.isEmpty())
        advanceTimeBy(1000)
        runCurrent()
        saving.complete(Unit)
        runCurrent()
        assertFalse(vm.state.value.saving)
        assertEquals(otherAddress, vm.state.value.deviceId)
        assertEquals("Renamed first computer", vm.state.value.devices.first { it.id == address }.name)
        val completed = vm.state.value
        coEvery { other.connect() } returns RemoteDeviceStatus.CONNECTING
        coVerify(exactly = 1) { other.connect() }
        advanceTimeBy(1999)
        runCurrent()
        coVerify(exactly = 1) { other.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(completed.copy(devices = completed.devices.map {
            if (it.id == otherAddress) it.copy(desktopStatus = RemoteDeviceStatus.CONNECTING) else it
        }), vm.state.value)
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 3) { other.connect() }
        coVerify(exactly = 1) { other.sessions(any()) }
        coVerify(exactly = 1) { other.models() }
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { replacement.connect() }
        coVerify(exactly = 1) { connections.save(any(), address) }
        coVerify(exactly = 0) { client.sessions(any()); replacement.sessions(any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()); other.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun nativeRenameCompletionResumesDesktopObservationAfterAFullIntervalWithoutVisibleRows() = runTest(dispatcher) {
        val vm = openCatalog()
        val renamed = session.copy(title = "Native renamed title", updatedAt = 2)
        val pending = CompletableDeferred<RemoteSession>(backgroundScope.coroutineContext[Job])
        coEvery { client.rename(session.id, "Requested title") } coAnswers { pending.await() }
        advanceTimeBy(1000)
        runCurrent()
        vm.renameSession(session.id, "Requested title")
        runCurrent()
        assertTrue(vm.state.value.controlling)
        advanceTimeBy(6000)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        pending.complete(renamed)
        runCurrent()
        assertFalse(vm.state.value.controlling)
        assertEquals(listOf(renamed), vm.state.value.sessions)
        val completed = vm.state.value
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(completed.copy(devices = completed.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTING)
        }), vm.state.value)
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.rename(session.id, "Requested title") }
        vm.setVisible(false)
    }

    @Test fun archivingTheLastVisibleSessionResumesDesktopObservationForTheEmptyCatalog() = runTest(dispatcher) {
        val vm = openCatalog()
        vm.observeSessions(address, listOf(session.id))
        runCurrent()
        val pending = CompletableDeferred<Unit>(backgroundScope.coroutineContext[Job])
        coEvery { client.archiveSession(session.id) } coAnswers { pending.await() }
        advanceTimeBy(1000)
        runCurrent()
        vm.archiveSession(session.id)
        runCurrent()
        assertTrue(vm.state.value.controlling)
        advanceTimeBy(6000)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        pending.complete(Unit)
        runCurrent()
        assertFalse(vm.state.value.controlling)
        assertTrue(vm.state.value.sessions.isEmpty())
        val completed = vm.state.value
        coEvery { client.connect() } returns RemoteDeviceStatus.ERROR
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(completed.copy(devices = completed.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.ERROR)
        }), vm.state.value)
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.archiveSession(session.id) }
        vm.setVisible(false)
    }

    @Test fun slowObservationIsSerialAndSchedulesTheNextReadAfterItCompletes() = runTest(dispatcher) {
        val vm = openCatalog()
        val pending = CompletableDeferred<RemoteDeviceStatus>(backgroundScope.coroutineContext[Job])
        coEvery { client.connect() } coAnswers { pending.await() }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        pending.complete(RemoteDeviceStatus.CONNECTING)
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().desktopStatus)
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        coVerify(exactly = 3) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun hidingStopsObservationAndReentryWaitsAFullInterval() = runTest(dispatcher) {
        val vm = openCatalog()
        advanceTimeBy(1000)
        runCurrent()
        vm.setVisible(false)
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        vm.setVisible(true)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun returningToDevicesStopsPeriodicObservationAfterTheExplicitEntryCheck() = runTest(dispatcher) {
        val vm = openCatalog()
        vm.selectDevice(null)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        val before = vm.state.value
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        assertEquals(before, vm.state.value)
        vm.setVisible(false)
    }

    @Test fun deviceEditorStopsObservationWithoutSavingOrChecking() = runTest(dispatcher) {
        val vm = openCatalog()
        vm.editDevice(address)
        runCurrent()
        assertTrue(vm.state.value.addingDevice)
        val before = vm.state.value
        advanceTimeBy(9000)
        runCurrent()
        assertEquals(before, vm.state.value)
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 1) { connections.save(any(), any()) }
        vm.setVisible(false)
    }

    @Test fun visibleHistoryObservesDesktopWithoutChangingItsMessagesOrNativeReadiness() = runTest(dispatcher) {
        // This lifecycle regression retains native input without invoking the Markdown dispatcher.
        val message = RemoteMessage("input", "turn", null, "user", "Retained native input", 1)
        coEvery { client.conversation(session.id, null) } returns bodyPage(listOf(message), null, emptyList())
        val vm = openCatalog()
        vm.selectSession(session)
        runCurrent()
        val admitted = vm.state.value
        assertEquals(listOf(message.id), admitted.nodes.map { it.id })
        assertEquals("idle", admitted.runtime?.status)
        assertTrue(admitted.canChooseModel)
        assertTrue(admitted.canEditSettings)
        coVerify(exactly = 1) { client.connect() }
        for (desktop in listOf(RemoteDeviceStatus.IDLE, RemoteDeviceStatus.ERROR, RemoteDeviceStatus.CONNECTING)) {
            coEvery { client.connect() } returns desktop
            advanceTimeBy(3000)
            runCurrent()
            assertEquals(admitted.copy(devices = admitted.devices.map {
                it.copy(desktopStatus = desktop)
            }), vm.state.value)
        }
        coEvery { client.connect() } throws IOException("Desktop observation unavailable")
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(admitted.copy(devices = admitted.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.IDLE)
        }), vm.state.value)
        coVerify(exactly = 5) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.conversation(any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun lateCatalogObservationCannotChangeTheNewlyOpenedHistory() = runTest(dispatcher) {
        val vm = openCatalog()
        val stale = CompletableDeferred<RemoteDeviceStatus>(backgroundScope.coroutineContext[Job])
        coEvery { client.connect() } coAnswers { withContext(NonCancellable) { stale.await() } }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        vm.selectSession(session)
        runCurrent()
        val history = vm.state.value
        assertEquals(session, history.session)
        assertEquals("idle", history.runtime?.status)
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        stale.complete(RemoteDeviceStatus.ERROR)
        runCurrent()
        assertEquals(history, vm.state.value)
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(history.copy(devices = history.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTING)
        }), vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun localDraftObservesDesktopWithoutImmediateRequestsOrNativeMutations() = runTest(dispatcher) {
        coEvery { client.models() } returns listOf(RemoteModel("model", "Model", true,
            reasoningEfforts = listOf("low", "high"), defaultReasoningEffort = "low",
            serviceTiers = listOf(RemoteServiceTier("priority", "Priority"))))
        val vm = openCatalog()
        vm.newSession()
        vm.editDraft(requireNotNull(vm.state.value.owner), "Keep this draft")
        vm.setThinkingLevel("high")
        vm.setServiceTier("priority")
        runCurrent()
        assertTrue(vm.state.value.isDraft)
        assertEquals("high", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        val before = vm.state.value
        vm.newSession()
        vm.refresh()
        runCurrent()
        assertEquals(before, vm.state.value)
        coVerify(exactly = 1) { client.connect() }
        coEvery { client.connect() } returns RemoteDeviceStatus.IDLE
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(before.copy(devices = before.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.IDLE)
        }), vm.state.value)
        vm.setVisible(false)
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        vm.setVisible(true)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(before, vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        coVerify(exactly = 1) { client.sessions(any()) }
        coVerify(exactly = 1) { client.models() }
        coVerify(exactly = 0) { client.conversation(any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.updateSettings(any(), any()); client.setModel(any(), any()) }
        vm.setVisible(false)
    }

    @Test fun localDraftObservationCoalescesWithExplicitPendingGatewayCheck() = runTest(dispatcher) {
        val vm = openCatalog()
        vm.newSession()
        vm.editDraft(requireNotNull(vm.state.value.owner), "Unsent draft")
        runCurrent()
        val pending = CompletableDeferred<RemoteDeviceStatus>(backgroundScope.coroutineContext[Job])
        coEvery { client.connect() } coAnswers { pending.await() }
        vm.retryConnection()
        vm.retryConnection()
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().status)
        advanceTimeBy(9000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        pending.complete(RemoteDeviceStatus.IDLE)
        runCurrent()
        val reconnected = vm.state.value
        assertEquals(RemoteDeviceStatus.CONNECTED, reconnected.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, reconnected.devices.single().desktopStatus)
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(reconnected.copy(devices = reconnected.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
        }), vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        coVerify(exactly = 1) { client.models(); client.sessions(any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.updateSettings(any(), any()); client.setModel(any(), any()) }
        vm.setVisible(false)
    }

    private fun TestScope.rejectStaleObservation(fail: Boolean, draft: Boolean? = null) {
        val vm = openCatalog()
        if (draft == true) vm.newSession() else if (draft == false) vm.selectSession(session)
        runCurrent()
        val stale = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
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
        vm.setVisible(false)
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        vm.setVisible(true)
        runCurrent()
        advanceTimeBy(3000)
        runCurrent()
        assertEquals(RemoteDeviceStatus.CONNECTING, vm.state.value.devices.single().desktopStatus)
        val current = vm.state.value
        // Deliver a failed response through the client without failing the fixture's parent job.
        stale.complete(if (fail) Result.failure(IOException("Old observation failed"))
            else Result.success(RemoteDeviceStatus.CONNECTED))
        runCurrent()
        assertTrue("The stale client response must actually resume", staleResumed)
        assertEquals(current, vm.state.value)
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 4) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun lateSuccessFromHiddenCatalogCannotOverwriteNewObservation() = runTest(dispatcher) {
        rejectStaleObservation(fail = false)
    }

    @Test fun lateFailureFromHiddenCatalogCannotClearNewObservation() = runTest(dispatcher) {
        rejectStaleObservation(fail = true)
    }

    @Test fun directlyOpenedSessionObservesDesktopAfterGatewayAdmissionWithoutLoadingTheCatalog() = runTest(dispatcher) {
        coEvery { connections.load() } returns listOf(RemoteConnection("Computer", address, "token"))
        coEvery { client.connect() } returns RemoteDeviceStatus.IDLE
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        backgroundScope.launch(dispatcher, start = CoroutineStart.UNDISPATCHED) {
            try { awaitCancellation() } finally { vm.setVisible(false) }
        }
        runCurrent()
        vm.selectDevice(address)
        vm.selectSession(session)
        vm.setVisible(true)
        runCurrent()
        val admitted = vm.state.value
        assertEquals(session, admitted.session)
        assertEquals(RemoteDeviceStatus.CONNECTED, admitted.devices.single().status)
        assertEquals(RemoteDeviceStatus.IDLE, admitted.devices.single().desktopStatus)
        assertEquals("idle", admitted.runtime?.status)
        coVerify(exactly = 1) { client.connect() }
        coVerify(exactly = 0) { client.sessions(any()) }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 1) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(admitted.copy(devices = admitted.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTED)
        }), vm.state.value)
        coVerify(exactly = 2) { client.connect() }
        coVerify(exactly = 0) { client.sessions(any()) }
        coVerify(exactly = 0) { connections.save(any(), any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()); client.send(any(), any(), any(), any()) }
        vm.setVisible(false)
    }

    @Test fun openingAnotherSessionRejectsLateDesktopObservationAndStartsOneNewInterval() = runTest(dispatcher) {
        val vm = openCatalog()
        vm.selectSession(session)
        runCurrent()
        val stale = CompletableDeferred<Result<RemoteDeviceStatus>>(backgroundScope.coroutineContext[Job])
        var staleResumed = false
        coEvery { client.connect() } coAnswers { withContext(NonCancellable) {
            val result = stale.await()
            staleResumed = true
            result.getOrThrow()
        } }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTING
        vm.selectSession(session.copy(id = "second-session"))
        runCurrent()
        val selected = vm.state.value
        assertEquals("second-session", selected.session?.id)
        stale.complete(Result.failure(IOException("Old session observation failed")))
        runCurrent()
        assertTrue(staleResumed)
        assertEquals(selected, vm.state.value)
        advanceTimeBy(2999)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        advanceTimeBy(1)
        runCurrent()
        assertEquals(selected.copy(devices = selected.devices.map {
            it.copy(desktopStatus = RemoteDeviceStatus.CONNECTING)
        }), vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        vm.editDevice(address)
        runCurrent()
        val editor = vm.state.value
        advanceTimeBy(9000)
        runCurrent()
        assertEquals(editor, vm.state.value)
        coVerify(exactly = 3) { client.connect() }
        vm.setVisible(false)
    }

    @Test fun hiddenConversationRejectsLateFailureAndResumesObservationAfterReentry() = runTest(dispatcher) {
        rejectStaleObservation(fail = true, draft = false)
    }

    @Test fun hiddenLocalDraftRejectsLateSuccessAndResumesObservationAfterReentry() = runTest(dispatcher) {
        rejectStaleObservation(fail = false, draft = true)
    }

    @Test fun replacingCredentialsAtSameAddressRejectsTheOldChatsLateDesktopResult() = runTest(dispatcher) {
        val replacement = mockk<FiloClient>()
        every { replacement.adapters } returns null
        every { replacement.address } returns address
        coEvery { replacement.connect() } returns RemoteDeviceStatus.ERROR
        coEvery { replacement.sessions(any()) } returns RemoteSessionPage(emptyList(), null)
        coEvery { replacement.models() } returns emptyList()
        val vm = openCatalog(replacement)
        vm.selectSession(session)
        runCurrent()
        val stale = CompletableDeferred<RemoteDeviceStatus>(backgroundScope.coroutineContext[Job])
        coEvery { client.connect() } coAnswers { withContext(NonCancellable) { stale.await() } }
        advanceTimeBy(3000)
        runCurrent()
        coVerify(exactly = 2) { client.connect() }
        vm.editDevice(address)
        vm.saveDevice(address, "replacement", "Replacement device")
        runCurrent()
        vm.selectDevice(address)
        runCurrent()
        vm.newSession()
        runCurrent()
        val current = vm.state.value
        assertTrue(current.isDraft)
        assertEquals(RemoteDeviceStatus.CONNECTED, current.devices.single().status)
        assertEquals(RemoteDeviceStatus.ERROR, current.devices.single().desktopStatus)
        assertEquals("Replacement device", current.devices.single().name)
        stale.complete(RemoteDeviceStatus.CONNECTED)
        runCurrent()
        assertEquals(current, vm.state.value)
        coVerify(exactly = 1) { replacement.connect() }
        vm.setVisible(false)
    }
}
