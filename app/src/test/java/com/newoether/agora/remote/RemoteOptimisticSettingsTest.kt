package com.newoether.agora.remote

import io.mockk.*
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.awaitCancellation
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.test.*
import org.junit.After
import org.junit.Assert.*
import org.junit.Before
import org.junit.Test
import java.io.IOException

@OptIn(ExperimentalCoroutinesApi::class)
class RemoteOptimisticSettingsTest {
    private val dispatcher = StandardTestDispatcher()
    private val client = mockk<FiloClient>()
    private val store = mockk<RemoteConnectionStore>(relaxed = true)
    private val session = RemoteSession("session", "Existing", "/workspace", 1)
    private val model = RemoteModel("model", "Model", true, listOf("low", "high", "ultra"), "high",
        listOf(RemoteServiceTier("priority", "Priority")), null)
    private var runtime = RemoteRuntime("idle", model = "model", effort = "high", serviceTier = "default")

    @Before fun setup() {
        Dispatchers.setMain(dispatcher)
        coEvery { store.load() } returns emptyList()
        every { client.adapters } returns null
        every { client.address } returns "http://computer/"
        coEvery { client.connect() } returns RemoteDeviceStatus.CONNECTED
        coEvery { client.sessions(any()) } returns RemoteSessionPage(listOf(session), null)
        coEvery { client.models() } returns listOf(model, model.copy(id = "second-model"))
        coEvery { client.conversation(any(), any()) } answers { bodyPage(emptyList(), null, emptyList(), runtime) }
        every { client.events(any()) } answers {
            flow { emit(client.conversation(firstArg())); awaitCancellation() }
        }
    }
    @After fun teardown() { Dispatchers.resetMain() }

    private fun TestScope.open(): RemoteViewModel {
        val vm = RemoteViewModel(store, projectionDispatcher = dispatcher) { _, _ -> client }; runCurrent()
        vm.saveDevice("http://computer/", "token"); runCurrent()
        vm.selectDevice("http://computer/"); vm.setVisible(true); runCurrent()
        vm.selectSession(session); runCurrent()
        return vm
    }
    private fun applyNativeSettings(settings: RemoteSettings) {
        runtime = runtime.copy(effort = settings.effort ?: runtime.effort,
            serviceTier = if (settings.updateServiceTier) settings.serviceTier ?: "default" else runtime.serviceTier)
    }

    @Test fun immediateProjectionKeepsControlsUsableAndCoalescesUnsentChoices() = runTest(dispatcher) {
        val gate = CompletableDeferred<Unit>()
        coEvery { client.updateSettings(any(), any()) } coAnswers { gate.await(); applyNativeSettings(secondArg()) }
        val vm = open()
        vm.setThinkingLevel("ultra")
        vm.setThinkingLevel("low")
        vm.setServiceTier("priority")
        assertEquals("low", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        assertEquals("high", vm.state.value.runtime?.effort)
        assertTrue(vm.state.value.settingsPending)
        assertTrue(vm.state.value.canEditSettings)
        assertFalse(vm.state.value.canChooseModel)
        assertFalse(vm.state.value.controlling)
        runCurrent()
        coVerify(exactly = 1) {
            client.updateSettings("session", RemoteSettings(effort = "low", serviceTier = "priority", updateServiceTier = true))
        }
        vm.refresh(); runCurrent()
        assertEquals("low", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        val owner = vm.state.value.owner!!
        vm.editDraft(owner, "next turn"); vm.send(); vm.setModel("second-model"); runCurrent()
        coVerify(exactly = 0) { client.send(any(), any(), any(), any()) }
        coVerify(exactly = 0) { client.setModel(any(), any()) }
        gate.complete(Unit); runCurrent()
        assertFalse(vm.state.value.settingsPending)
        assertEquals("low", vm.state.value.runtime?.effort)
        assertEquals("priority", vm.state.value.runtime?.serviceTier)
        assertEquals("next turn", vm.state.value.drafts[owner])
        vm.setVisible(false)
    }

    @Test fun olderFailureCannotRollBackNewerChoiceAndWritesRemainSerial() = runTest(dispatcher) {
        val first = CompletableDeferred<Unit>(); val second = CompletableDeferred<Unit>()
        val writes = mutableListOf<RemoteSettings>()
        coEvery { client.updateSettings(any(), any()) } coAnswers {
            val settings = secondArg<RemoteSettings>(); writes += settings
            if (writes.size == 1) { first.await(); throw IOException("uncertain settings") }
            second.await(); applyNativeSettings(settings)
        }
        val vm = open()
        vm.setThinkingLevel("ultra"); runCurrent()
        vm.setThinkingLevel("low"); vm.setServiceTier("priority"); runCurrent()
        assertEquals(1, writes.size)
        assertEquals("low", vm.state.value.selectedEffort)
        first.complete(Unit); runCurrent()
        assertEquals(2, writes.size)
        assertEquals(RemoteSettings(effort = "low", serviceTier = "priority", updateServiceTier = true), writes[1])
        assertEquals("low", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        assertTrue(vm.state.value.canEditSettings)
        assertEquals("control_failed", vm.notices.first().stage)
        second.complete(Unit); runCurrent()
        assertFalse(vm.state.value.settingsPending)
        assertEquals("low", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        assertEquals(2, writes.size)
        vm.setVisible(false)
    }

    @Test fun failureRestoresOnlyFailedFieldWhileOtherPendingFieldStaysOptimistic() = runTest(dispatcher) {
        val first = CompletableDeferred<Unit>(); val second = CompletableDeferred<Unit>()
        var writes = 0
        coEvery { client.updateSettings(any(), any()) } coAnswers {
            writes++
            if (writes == 1) { first.await(); throw IOException("lost response") }
            second.await(); applyNativeSettings(secondArg())
        }
        val vm = open()
        val revision = vm.state.value.settingsRevision
        vm.setThinkingLevel("ultra"); runCurrent()
        vm.setServiceTier("priority"); runCurrent()
        first.complete(Unit); runCurrent()
        assertEquals("high", vm.state.value.selectedEffort)
        assertEquals("priority", vm.state.value.selectedServiceTier)
        assertEquals(revision + 1, vm.state.value.settingsRevision)
        assertTrue(vm.state.value.settingsPending)
        second.complete(Unit); runCurrent()
        assertFalse(vm.state.value.settingsPending)
        assertEquals("high", vm.state.value.runtime?.effort)
        coVerify(exactly = 2) { client.updateSettings(any(), any()) }
        vm.setVisible(false)
    }

    @Test fun explicitDefaultIsKnownImmediatelyThenReturnsToUnknownOnFailure() = runTest(dispatcher) {
        runtime = runtime.copy(serviceTier = null, serviceTierKnown = false)
        val gate = CompletableDeferred<Unit>()
        coEvery { client.updateSettings(any(), any()) } coAnswers { gate.await(); throw IOException("lost response") }
        val vm = open()
        assertFalse(vm.state.value.serviceTierKnown)
        vm.setServiceTier(null)
        assertTrue(vm.state.value.serviceTierKnown)
        assertNull(vm.state.value.selectedServiceTier)
        runCurrent(); gate.complete(Unit); runCurrent()
        assertFalse(vm.state.value.serviceTierKnown)
        assertFalse(vm.state.value.settingsPending)
        assertEquals("control_failed", vm.notices.first().stage)
        coVerify(exactly = 1) { client.updateSettings("session", RemoteSettings(updateServiceTier = true)) }
        vm.setVisible(false)
    }

    @Test fun returningToPendingOwnerKeepsControlsAndReportsFailureWithRollback() = runTest(dispatcher) {
        val gate = CompletableDeferred<Unit>()
        coEvery { client.updateSettings("session", any()) } coAnswers {
            gate.await(); throw IOException("lost response")
        }
        val vm = open()
        vm.setThinkingLevel("ultra"); runCurrent()
        vm.selectSession(session.copy(id = "second")); runCurrent()
        vm.selectSession(session); runCurrent()
        assertEquals("model", vm.state.value.settingsModel?.id)
        assertEquals("ultra", vm.state.value.selectedEffort)
        assertTrue(vm.state.value.settingsPending)
        assertTrue(vm.state.value.canEditSettings)
        val revision = vm.state.value.settingsRevision
        gate.complete(Unit); runCurrent()
        assertEquals("high", vm.state.value.selectedEffort)
        assertFalse(vm.state.value.settingsPending)
        assertTrue(vm.state.value.canEditSettings)
        assertTrue(vm.state.value.settingsRevision > revision)
        assertEquals("control_failed", vm.notices.first().stage)
        coVerify(exactly = 1) { client.updateSettings("session", RemoteSettings(effort = "ultra")) }
        vm.setVisible(false)
    }

    @Test fun lateFailureDoesNotChangeAnotherOwnersProjectionOrReportItsErrorThere() = runTest(dispatcher) {
        val first = CompletableDeferred<Unit>(); val second = CompletableDeferred<Unit>()
        coEvery { client.updateSettings("session", any()) } coAnswers { first.await(); throw IOException("old failure") }
        coEvery { client.updateSettings("second", any()) } coAnswers { second.await(); applyNativeSettings(secondArg()) }
        val vm = open()
        vm.setThinkingLevel("ultra"); runCurrent()
        vm.selectSession(session.copy(id = "second")); runCurrent()
        vm.setThinkingLevel("low"); runCurrent()
        val revision = vm.state.value.settingsRevision
        first.complete(Unit); runCurrent()
        assertEquals("second", vm.state.value.session?.id)
        assertEquals("low", vm.state.value.selectedEffort)
        assertEquals(revision, vm.state.value.settingsRevision)
        assertTrue(vm.state.value.settingsPending)
        assertFalse(vm.state.value.error)
        second.complete(Unit); runCurrent()
        assertEquals("low", vm.state.value.selectedEffort)
        assertFalse(vm.state.value.settingsPending)
        coVerify(exactly = 2) { client.updateSettings(any(), any()) }
        vm.setVisible(false)
    }
}
