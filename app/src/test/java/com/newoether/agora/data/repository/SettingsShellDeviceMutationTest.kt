package com.newoether.agora.data.repository

import android.content.Context
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.SettingsShellDeviceStore
import com.newoether.agora.data.ShellDeviceConfig
import io.mockk.coEvery
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.CoroutineExceptionHandler
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.test.StandardTestDispatcher
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import org.junit.Assert.*
import org.junit.Test
import java.io.IOException

class SettingsShellDeviceMutationTest {
    private val device = ShellDeviceConfig(id = "device", name = "Build Host")

    @Test
    fun storageFailureReportsExactlyOnceAndDoesNotEscapeTheRepositoryJob() = runTest {
        val storage = mockk<SettingsShellDeviceStore>()
        val repository = mockk<SettingsRepository>()
        val uncaught = mutableListOf<Throwable>()
        val scope = CoroutineScope(SupervisorJob() + StandardTestDispatcher(testScheduler) +
            CoroutineExceptionHandler { _, failure -> uncaught += failure })
        val manager = mockk<SettingsManager>()
        every { repository.scope } returns scope
        every { repository.settingsManager } returns manager
        every { manager.shellDeviceStore } returns storage
        val failure = IOException("Storage unavailable")
        coEvery { storage.add(device) } throws failure
        coEvery { storage.update(device) } throws failure
        try {
            val results = mutableListOf<Boolean>()
            val failures = mutableListOf<Exception>()
            for (creating in listOf(true, false)) {
                val job = repository.persistShellDevice(device, creating, results::add, failures::add)
                runCurrent()
                job.join()
                assertTrue(job.isCompleted)
                assertFalse(job.isCancelled)
            }
            assertEquals(listOf(failure, failure), failures)
            assertTrue(results.isEmpty())
            assertTrue(uncaught.isEmpty())
        } finally {
            scope.cancel()
        }
    }

    @Test
    fun cancellationPropagatesWithoutFailureOrBooleanCompletion() = runTest {
        val storage = mockk<SettingsShellDeviceStore>()
        val manager = mockk<SettingsManager>()
        val repository = mockk<SettingsRepository>()
        every { repository.scope } returns backgroundScope
        every { repository.settingsManager } returns manager
        every { manager.shellDeviceStore } returns storage
        coEvery { storage.add(device) } throws CancellationException("Cancelled")
        coEvery { storage.update(device) } throws CancellationException("Cancelled")
        var callbacks = 0
        for (creating in listOf(true, false)) {
            val job = repository.persistShellDevice(device, creating, { callbacks++ }, { callbacks++ })
            runCurrent()
            job.join()
            assertTrue(job.isCancelled)
        }
        assertEquals(0, callbacks)
    }

    @Test
    fun successAndRejectedAdmissionKeepTheExistingBooleanContract() = runTest {
        val storage = mockk<SettingsShellDeviceStore>()
        val manager = mockk<SettingsManager>()
        val repository = mockk<SettingsRepository>()
        every { repository.scope } returns backgroundScope
        every { repository.settingsManager } returns manager
        every { manager.shellDeviceStore } returns storage
        coEvery { storage.add(device) } returns true
        coEvery { storage.update(device) } returns false
        val results = mutableListOf<Boolean>()
        val failures = mutableListOf<Exception>()
        val add = repository.persistShellDevice(device, true, results::add, failures::add)
        val update = repository.persistShellDevice(device, false, results::add, failures::add)
        runCurrent()
        add.join()
        update.join()
        assertEquals(listOf(true, false), results)
        assertTrue(failures.isEmpty())
    }

    @Test
    fun oldCallersStillReceiveFalseOnStorageFailureAndExplicitErrorCallbackIsPreserved() = runTest {
        val storage = mockk<SettingsShellDeviceStore>()
        val manager = mockk<SettingsManager>()
        val repository = mockk<SettingsRepository>()
        every { repository.scope } returns backgroundScope
        every { repository.settingsManager } returns manager
        every { manager.shellDeviceStore } returns storage
        every { repository.addShellDevice(any(), any(), any()) } answers { callOriginal() }
        every { repository.updateShellDevice(any(), any(), any()) } answers { callOriginal() }
        coEvery { storage.add(device) } throws IOException("Full")
        coEvery { storage.update(device) } throws IOException("Full")
        val booleans = mutableListOf<Boolean>()
        val failures = mutableListOf<Exception>()
        repository.addShellDevice(device, booleans::add).join()
        repository.updateShellDevice(device, booleans::add, failures::add).join()
        assertEquals(listOf(false), booleans)
        assertEquals(1, failures.size)
    }
}
