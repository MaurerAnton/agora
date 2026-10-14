package com.newoether.agora.sandbox

import android.content.Context
import android.system.Os
import com.newoether.agora.R
import com.newoether.agora.data.repository.SettingsRepository
import io.mockk.Runs
import io.mockk.every
import io.mockk.just
import io.mockk.mockk
import io.mockk.mockkStatic
import io.mockk.unmockkStatic
import java.io.File
import java.nio.file.Files
import java.nio.file.StandardCopyOption
import java.util.concurrent.CountDownLatch
import java.util.concurrent.TimeUnit
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.async
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ProotSandboxMutationTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun resetWaitsForAnInFlightAtomicFileWriteBeforeRemovingRootfs() = runBlocking {
        val directory = temporaryFolder.newFolder("sandbox-mutation")
        val marker = File(directory, "alpine-rootfs/etc/marker")
        marker.parentFile!!.mkdirs()
        marker.writeText("rootfs must remain intact while a write owns it")
        val context = mockk<Context>()
        every { context.filesDir } returns directory
        every { context.getString(R.string.sandbox_snackbar_reset) } returns "Reset"
        val manager = ProotSandboxManager(context, mockk<SettingsRepository>(relaxed = true))
        val replacing = CountDownLatch(1)
        val releaseWrite = CountDownLatch(1)
        mockkStatic(Os::class)
        try {
            every { Os.chmod(any(), any()) } just Runs
            every { Os.rename(any(), any()) } answers {
                replacing.countDown()
                check(releaseWrite.await(5, TimeUnit.SECONDS)) { "Write barrier timed out" }
                Files.move(
                    File(firstArg<String>()).toPath(),
                    File(secondArg<String>()).toPath(),
                    StandardCopyOption.REPLACE_EXISTING,
                )
                Unit
            }
            val write = async(Dispatchers.IO) { manager.fileWrite("/etc/new-file", "complete bytes") }
            try {
                assertTrue("The real file operation reached atomic replacement", replacing.await(5, TimeUnit.SECONDS))
                val reset = async(Dispatchers.IO) { manager.reset() }
                try {
                    assertNull("Reset must wait for the existing writer", withTimeoutOrNull(250) { reset.await() })
                    assertTrue("Reset must not delete the writer's rootfs", marker.isFile)
                    releaseWrite.countDown()
                    assertNull(write.await())
                    assertTrue(reset.await())
                } finally {
                    releaseWrite.countDown()
                    reset.join()
                }
            } finally {
                releaseWrite.countDown()
                write.join()
            }
        } finally {
            releaseWrite.countDown()
            unmockkStatic(Os::class)
        }
    }
}
