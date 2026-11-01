package com.newoether.agora.remote

import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.mockk
import java.io.IOException
import kotlinx.coroutines.CompletableDeferred
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
internal class RemoteUsageReadTest : RemoteViewModelFixture() {
    private val usage = RemoteUsage(listOf(RemoteUsageLimit("codex", "Codex",
        primary = RemoteUsageWindow(49.0, 10080, 1793082762))))

    @Test fun legacyDeviceReadsAccountUsageWithoutOpeningASession() = runTest(dispatcher) {
        coEvery { client.usage() } returns usage
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        runCurrent()
        saveAndSelect(vm)
        assertNull(vm.state.value.session)
        assertEquals(usage, vm.usage())
        coVerify(exactly = 1) { client.usage() }
        coVerify(exactly = 0) { client.sessions(any()) }
        coVerify(exactly = 0) { client.create(any(), any(), any(), any()) }
    }

    @Test fun selectedSourceReadsOnlyItsOwnAccountAndRejectsLateDeviceResults() = runTest(dispatcher) {
        val desktop = RemoteAdapter("codex-desktop", "codex", "desktop", "connected",
            nativeScopeId = "account-a", capabilities = RemoteCapabilities(usage = true))
        val scoped = mockk<FiloClient>()
        val pending = CompletableDeferred<RemoteUsage>()
        every { client.adapters } returns listOf(desktop)
        every { client.scoped(RemoteAccessPath(desktop.id, nativeScopeId = desktop.nativeScopeId)) } returns scoped
        coEvery { scoped.usage() } coAnswers { pending.await() }
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        runCurrent()
        saveAndSelect(vm)
        val result = async { vm.usage() }
        runCurrent()
        vm.selectDevice(null)
        pending.complete(usage)
        runCurrent()
        assertNull(result.await())
        coVerify(exactly = 1) { scoped.usage() }
        coVerify(exactly = 0) { client.usage() }
    }

    @Test fun failedAccountReadRemainsUnavailableAndDoesNotChangeDeviceHealth() = runTest(dispatcher) {
        coEvery { client.usage() } throws IOException("Fixture read failure")
        val vm = RemoteViewModel(connections, projectionDispatcher = dispatcher) { _, _ -> client }
        runCurrent()
        saveAndSelect(vm)
        assertNull(vm.usage())
        assertEquals(RemoteDeviceStatus.CONNECTED, vm.state.value.devices.single().status)
        coVerify(exactly = 1) { client.usage() }
    }
}
