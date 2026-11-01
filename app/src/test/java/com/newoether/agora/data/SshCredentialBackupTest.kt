package com.newoether.agora.data

import com.newoether.agora.util.SecretCrypto
import io.mockk.*
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.flowOf
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import org.junit.Assert.*
import org.junit.Test

class SshCredentialBackupTest {
    private val json = Json { ignoreUnknownKeys = true }
    private val local = ShellDeviceConfig(
        id = "stable-device", name = "Renamed server", type = "ssh", sshAuthMode = SSH_AUTH_PRIVATE_KEY,
        sshPrivateKeyName = "id_rsa", sshPrivateKey = "local-key", sshKeyPassphrase = "local-passphrase",
        sshPassword = "local-password", apiKey = "local-api-key",
    )

    @Test
    fun absentAuthenticationFieldStillMeansPassword() {
        val legacy = json.decodeFromString<ShellDeviceConfig>("""{"name":"legacy","type":"ssh","sshPassword":"secret"}""")
        assertEquals(SSH_AUTH_PASSWORD, legacy.sshAuthMode)
        assertEquals("secret", legacy.sshPassword)
        assertEquals("", legacy.sshPrivateKey)
    }

    @Test
    fun captureUsesStableIdsAndSecretRestoreHonorsMergeReplaceAndLegacyRecords() = runTest {
        val devices = MutableStateFlow(listOf(local, local.copy(id = "unmatched", name = "Other")))
        val manager = manager(devices)
        val capture = NativeBackupSecretsPolicy.capture(manager)
        assertEquals("local-key", capture.shellDevices.getValue(local.id).sshPrivateKey)
        assertFalse(capture.shellDevices.containsKey(local.name))
        val imported = NativeBackupSecrets(shellDevices = mapOf(
            local.id to ShellDeviceSecrets(sshPrivateKey = "imported-key", sshKeyPassphrase = "imported-passphrase"),
            "orphan" to ShellDeviceSecrets(sshPrivateKey = "orphan-key"),
        ))
        val warnings = NativeBackupSecretsPolicy.restore(imported, manager, replace = false)
        assertEquals("imported-key", devices.value.first().sshPrivateKey)
        assertEquals("imported-passphrase", devices.value.first().sshKeyPassphrase)
        assertEquals("local-key", devices.value.last().sshPrivateKey)
        assertTrue(warnings.single().contains("1 shell credential"))
        assertEquals(2, devices.value.size)
        val old = json.decodeFromString<NativeBackupSecrets>("""{"shellDevices":{"stable-device":{"sshPassword":"old-password"}}}""")
        NativeBackupSecretsPolicy.restore(old, manager, replace = false)
        assertEquals("imported-key", devices.value.first().sshPrivateKey)
        assertEquals("imported-passphrase", devices.value.first().sshKeyPassphrase)
        assertEquals("old-password", devices.value.first().sshPassword)
        NativeBackupSecretsPolicy.restore(old, manager, replace = true)
        assertEquals("", devices.value.first().sshPrivateKey)
        assertEquals("", devices.value.first().sshKeyPassphrase)
        assertEquals("", devices.value.last().sshPrivateKey)
        devices.value = listOf(local)
        val legacy = NativeBackupSecrets(shellApiKeys = mapOf(local.name to "legacy-api-key"))
        NativeBackupSecretsPolicy.restore(legacy, manager, replace = false)
        assertEquals("local-key", devices.value.single().sshPrivateKey)
        assertEquals("legacy-api-key", devices.value.single().apiKey)
        NativeBackupSecretsPolicy.restore(legacy, manager, replace = true)
        assertEquals("", devices.value.single().sshPrivateKey)
        assertEquals("", devices.value.single().sshKeyPassphrase)
    }

    @Test
    fun explicitEmptySecretClearsButAnAbsentFieldPreservesDuringMerge() = runTest {
        val devices = MutableStateFlow(listOf(local))
        val manager = manager(devices)
        NativeBackupSecretsPolicy.restore(
            NativeBackupSecrets(shellDevices = mapOf(local.id to ShellDeviceSecrets(sshPrivateKey = "", sshKeyPassphrase = ""))),
            manager, replace = false,
        )
        assertEquals("", devices.value.single().sshPrivateKey)
        assertEquals("", devices.value.single().sshKeyPassphrase)
    }

    @Test
    fun structuralImportNeverAcceptsPrivateKeyBytesEvenUnderLegacySecretPermission() = runTest {
        val devices = MutableStateFlow(listOf(local))
        val manager = manager(devices)
        val malicious = local.copy(sshPrivateKey = "settings-key", sshKeyPassphrase = "settings-passphrase")
        val archive = json.parseToJsonElement("""{"shellDevices":${json.encodeToString(listOf(malicious))}}""") as JsonObject
        for (replace in listOf(false, true)) {
            for (legacy in listOf(false, true)) {
                devices.value = listOf(local)
                PortableSettingsArchive.restoreFromJsonObject(archive, manager, replace, legacy, null) { it }
                assertEquals("local-key", devices.value.single().sshPrivateKey)
                assertEquals("local-passphrase", devices.value.single().sshKeyPassphrase)
                assertEquals(SSH_AUTH_PRIVATE_KEY, devices.value.single().sshAuthMode)
                devices.value = emptyList()
                PortableSettingsArchive.restoreFromJsonObject(archive, manager, replace, legacy, null) { it }
                assertEquals("", devices.value.single().sshPrivateKey)
                assertEquals("", devices.value.single().sshKeyPassphrase)
            }
        }
    }

    @Test
    fun keyBearingCompositeRequiresEncryptedEnvelopeAndRoundTripsInsideIt() {
        mockkObject(SecretCrypto)
        try {
            every { SecretCrypto.encrypt(any()) } answers { firstArg<String>() }
            every { SecretCrypto.isEncrypted(any()) } answers { firstArg<String>().startsWith("enc:v1:") }
            assertTrue(runCatching { encryptShellDevices(listOf(local), json) }.isFailure)
            every { SecretCrypto.encrypt(any()) } answers { "enc:v1:" + firstArg<String>().reversed() }
            every { SecretCrypto.decrypt(any()) } answers { firstArg<String>().removePrefix("enc:v1:").reversed() }
            val stored = encryptShellDevices(listOf(local), json)
            assertFalse(stored.contains("local-key"))
            val preferences = androidx.datastore.preferences.core.mutablePreferencesOf(SHELL_DEVICES_JSON to stored)
            assertEquals(listOf(local), decodeEncryptedShellDevices(preferences, json))
        } finally {
            unmockkObject(SecretCrypto)
        }
    }

    private fun manager(devices: MutableStateFlow<List<ShellDeviceConfig>>): SettingsManager {
        val manager = mockk<SettingsManager>(relaxed = true)
        every { manager.shellDevices } returns devices
        every { manager.apiKeys } returns flowOf(emptyList())
        every { manager.activeApiKeyIds } returns flowOf(emptyMap())
        every { manager.webSearchApiKeys } returns flowOf(emptyMap())
        every { manager.proxyPassword } returns flowOf("")
        every { manager.mcpServers } returns flowOf(emptyList())
        every { manager.embeddingModels } returns flowOf(emptyList())
        every { manager.activeEmbeddingModelId } returns flowOf("")
        every { manager.customFontPath } returns flowOf("")
        every { manager.customFontName } returns flowOf("")
        every { manager.customProviders } returns flowOf(emptyList())
        coEvery { manager.saveShellDevices(any()) } answers { devices.value = firstArg() }
        return manager
    }
}
