package com.newoether.agora.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import com.newoether.agora.util.SecretCrypto
import io.mockk.every
import io.mockk.mockkObject
import io.mockk.unmockkObject
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ShellDeviceNameTest {
    private val json = Json { ignoreUnknownKeys = true }

    @Test
    fun namesMatchToolRoutingIgnoreCaseTrimAndExcludeTheEditedIdentity() {
        val existing = ShellDeviceConfig(id = "existing", name = " Quantum ")
        assertFalse(isShellDeviceNameAvailable(ShellDeviceConfig(name = "quantum"), listOf(existing)))
        assertFalse(isShellDeviceNameAvailable(ShellDeviceConfig(name = " QUANTUM "), listOf(existing)))
        assertTrue(isShellDeviceNameAvailable(existing.copy(name = "quantum"), listOf(existing)))
        assertTrue(isShellDeviceNameAvailable(ShellDeviceConfig(name = "Quantum Work"), listOf(existing)))
        for (name in listOf("", "  ", "Local Sandbox", " local sandbox ")) {
            assertFalse(isShellDeviceNameAvailable(ShellDeviceConfig(name = name), emptyList()))
        }
    }

    @Test
    fun competingCreatesAdmitOnlyOneNameAndKeepUnrelatedPreferences() = runTest {
        withStore { store, devices ->
            store.edit { it[ASK_USER_ENABLED] = false }
            val results = listOf("Quantum", " quantum ").map { name ->
                async { devices.add(ShellDeviceConfig(name = name, apiKey = "credential")) }
            }.awaitAll()
            assertEquals(1, results.count { it })
            val saved = decodeEncryptedShellDevices(store.data.first(), json)
            assertEquals(1, saved.size)
            assertEquals(saved.single().name.trim(), saved.single().name)
            assertEquals("credential", saved.single().apiKey)
            assertEquals(false, store.data.first()[ASK_USER_ENABLED])
        }
    }

    @Test
    fun updateRejectsConflictsAndStaleIdsWithoutChangingStoredDevices() = runTest {
        withStore { store, devices ->
            val first = ShellDeviceConfig(id = "a", name = "Quantum", apiKey = "first-key")
            val second = ShellDeviceConfig(id = "b", name = "tinybox", type = "ssh", sshPassword = "password")
            assertTrue(devices.add(first))
            assertTrue(devices.add(second))
            val before = store.data.first().asMap()
            assertFalse(devices.add(first.copy(name = "Other")))
            assertFalse(devices.update(second.copy(name = " QUANTUM ")))
            assertFalse(devices.update(first.copy(id = "removed", name = "New")))
            assertEquals(before, store.data.first().asMap())
            assertTrue(devices.update(first.copy(name = " quantum ", description = "updated")))
            val saved = decodeEncryptedShellDevices(store.data.first(), json)
            assertEquals(listOf("quantum", "tinybox"), saved.map { it.name })
            assertEquals("first-key", saved.first().apiKey)
            assertEquals(second, saved.last())
            devices.remove(first.id)
            assertEquals(listOf(second), decodeEncryptedShellDevices(store.data.first(), json))
            assertFalse(devices.update(first.copy(name = "New")))
            assertEquals(listOf(second), decodeEncryptedShellDevices(store.data.first(), json))
        }
    }

    @Test
    fun independentConcurrentCreatesDoNotOverwriteEachOther() = runTest {
        withStore { store, devices ->
            val results = (1..12).map { index ->
                async { devices.add(ShellDeviceConfig(id = "$index", name = "Server $index")) }
            }.awaitAll()
            assertTrue(results.all { it })
            assertEquals((1..12).map { "$it" }.toSet(),
                decodeEncryptedShellDevices(store.data.first(), json).map { it.id }.toSet())
        }
    }

    private suspend fun withStore(block: suspend (DataStore<Preferences>, SettingsShellDeviceStore) -> Unit) {
        mockkObject(SecretCrypto)
        every { SecretCrypto.encrypt(any()) } answers { firstArg<String>() }
        every { SecretCrypto.decrypt(any()) } answers { firstArg<String>() }
        try {
            val store = object : DataStore<Preferences> {
                private val state = MutableStateFlow(emptyPreferences())
                private val mutex = Mutex()
                override val data: Flow<Preferences> = state
                override suspend fun updateData(transform: suspend (Preferences) -> Preferences): Preferences =
                    mutex.withLock { transform(state.value).also { state.value = it } }
            }
            block(store, SettingsShellDeviceStore(store, json))
        } finally {
            unmockkObject(SecretCrypto)
        }
    }
}
