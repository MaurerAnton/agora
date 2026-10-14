package com.newoether.agora.sandbox

import android.content.Context
import com.newoether.agora.R
import com.newoether.agora.data.repository.SettingsRepository
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.take
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.test.runTest
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.withTimeoutOrNull
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Assert.assertSame
import org.junit.Assert.assertTrue
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder

class ProotSandboxSnackbarEventTest {
    @get:Rule
    val temporaryFolder = TemporaryFolder()

    @Test
    fun fdroidFactoryReturnsOneManagerToAllProcessConsumers() {
        val context = mockk<Context>()
        every { context.filesDir } returns temporaryFolder.newFolder("shared-sandbox")
        val factory = FdroidSandboxManagerFactory(
            context = context,
            settings = mockk<SettingsRepository>(relaxed = true),
        )

        assertSame(factory.create(), factory.create())
    }

    @Test
    fun bufferedOutcomesAreOrderedConsumedOnceAndNotReplayed() = runTest {
        val context = mockk<Context>()
        every { context.filesDir } returns temporaryFolder.newFolder("sandbox")
        every { context.getString(R.string.sandbox_snackbar_reset) } returnsMany
            listOf("first", "second", "third")
        val manager = ProotSandboxManager(
            context = context,
            settings = mockk<SettingsRepository>(relaxed = true),
        )

        assertTrue(manager.reset())

        assertEquals("first", withTimeout(1_000) { manager.snackbarMessage.first() })
        assertNull(withTimeoutOrNull(100) { manager.snackbarMessage.first() })

        assertTrue(manager.reset())
        assertTrue(manager.reset())
        assertEquals(
            listOf("second", "third"),
            withTimeout(1_000) { manager.snackbarMessage.take(2).toList() },
        )
        assertNull(withTimeoutOrNull(100) { manager.snackbarMessage.first() })
    }
}
