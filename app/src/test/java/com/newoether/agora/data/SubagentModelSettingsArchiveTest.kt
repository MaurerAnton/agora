package com.newoether.agora.data

import android.content.Context
import androidx.datastore.core.okio.OkioStorage
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.PreferencesSerializer
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.util.SecretCrypto
import io.mockk.*
import java.nio.file.Files
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.*
import okio.FileSystem
import okio.Path.Companion.toPath
import org.junit.Assert.*
import org.junit.Test

class SubagentModelSettingsArchiveTest {
    @Test fun actualStorageRoundTripPreservesNullEmptyRemappingAndMergeReplaceRules() = runTest {
        withSettings { manager, context ->
            manager.saveEnabledModels(setOf("P:a", "P:b"))
            var exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonNull, exported["subagentEnabledModels"])
            assertEquals(JsonNull, exported["subagentRecommendedModel"])
            assertEquals(emptySet<String>(), manager.subagentModelIds.first())
            restore(manager, """{"subagentRecommendedModel":"P:a"}""")
            assertNull(manager.subagentRecommendedModel.first())
            val unconfigured = SettingsManager(context)
            assertNull(unconfigured.subagentEnabledModels.first())
            assertEquals(emptySet<String>(), unconfigured.subagentModelIds.first())
            restore(manager, """{"subagentEnabledModels":["P:b"],"subagentRecommendedModel":"P:b"}""")
            assertEquals(setOf("P:b"), manager.subagentEnabledModels.first())
            assertEquals("P:b", manager.subagentRecommendedModel.first())
            restore(manager, "{}")
            restore(manager, """{"subagentEnabledModels":null,"subagentRecommendedModel":null}""")
            assertEquals(setOf("P:b"), manager.subagentEnabledModels.first())
            assertEquals("P:b", manager.subagentRecommendedModel.first())
            restore(manager, """{"subagentEnabledModels":["P:a"],"subagentRecommendedModel":"P:missing"}""")
            assertEquals(setOf("P:a", "P:b"), manager.subagentEnabledModels.first())
            assertEquals("P:b", manager.subagentRecommendedModel.first())
            manager.saveLocalLowContextModeEnabled(true)
            restore(manager, """{"enabledModels":["P:a","P:b"]}""", replace = true)
            assertNull(manager.subagentEnabledModels.first())
            assertNull(manager.subagentRecommendedModel.first())
            assertEquals(emptySet<String>(), manager.subagentModelIds.first())
            assertEquals(setOf("P:a", "P:b"), manager.enabledModels.first())
            assertTrue(manager.localLowContextModeEnabled.first())
            restore(manager, """{"enabledModels":["P:a"],"subagentEnabledModels":null,"subagentRecommendedModel":"P:a"}""", true)
            assertNull(manager.subagentEnabledModels.first())
            assertEquals(emptySet<String>(), manager.subagentModelIds.first())
            assertNull(manager.subagentRecommendedModel.first())
            restore(manager, """{"enabledModels":["P:a"],"subagentEnabledModels":[],"subagentRecommendedModel":"P:a"}""", true)
            assertEquals(emptySet<String>(), manager.subagentEnabledModels.first())
            assertNull(manager.subagentRecommendedModel.first())
            exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonArray(emptyList()), exported["subagentEnabledModels"])
            restore(manager, "{}", true)
            PortableSettingsArchive.restoreFromJsonObject(exported, manager, true, false, null) { it }
            assertEquals(emptySet<String>(), manager.subagentEnabledModels.first())
            assertEquals(setOf("P:a"), manager.enabledModels.first())

            val id = "custom-provider-00000000-0000-4000-8000-000000000001"
            manager.saveCustomProviders(listOf(CustomProviderConfig("Relay", id = id)))
            manager.saveSubagentEnabledModels(null)
            restore(manager, """{"customProviders":[{"name":"Relay"}],"enabledModels":["Relay:model"],"subagentEnabledModels":["Relay:model"],"subagentRecommendedModel":"Relay:model"}""")
            assertEquals(setOf("$id:model"), manager.subagentEnabledModels.first())
            assertEquals("$id:model", manager.subagentRecommendedModel.first())
            exported = PortableSettingsArchive.toJsonObject(manager, false)
            PortableSettingsArchive.restoreFromJsonObject(exported, manager, true, false, null) { it }
            assertEquals(setOf("$id:model"), manager.subagentEnabledModels.first())
            assertEquals(setOf("$id:model"), manager.subagentModelIds.first())
            assertEquals("$id:model", manager.subagentRecommendedModel.first())
            val configuredReopened = SettingsManager(context)
            assertEquals(setOf("$id:model"), configuredReopened.subagentEnabledModels.first())
            assertEquals(setOf("$id:model"), configuredReopened.subagentModelIds.first())
        }
    }

    @Test fun repositoryPublishesRawConfigAndAtomicActionsRejectStaleChoicesWithoutCrashing() = runTest {
        withSettings { manager, context ->
            manager.saveEnabledModels(setOf("P:a", "P:b", "P:c"))
            val scope = CoroutineScope(backgroundScope.coroutineContext)
            try {
                val repository = SettingsRepository(manager, scope)
                repository.awaitInitialLoad()
                assertNull(repository.subagentEnabledModels.value)
                assertEquals(emptySet<String>(), repository.getSubagentModelIds())
                repository.setSubagentRecommendedModel("P:a").join()
                assertNull(manager.subagentRecommendedModel.first())
                repository.addSubagentModels(setOf("P:a")).join()
                assertEquals(setOf("P:a"), manager.subagentEnabledModels.first())
                val first = repository.addSubagentModels(setOf("P:a"))
                val second = repository.addSubagentModels(setOf("P:b"))
                first.join()
                second.join()
                assertEquals(setOf("P:a", "P:b"), manager.subagentEnabledModels.first())
                repository.setSubagentRecommendedModel("P:b").join()
                val remove = repository.removeSubagentModel("P:b")
                remove.join()
                repository.setSubagentRecommendedModel("P:b").join()
                assertNull(manager.subagentRecommendedModel.first())
                manager.saveEnabledModels(setOf("P:a"))
                repository.addSubagentModels(setOf("P:c")).join()
                repository.setSubagentRecommendedModel("P:c").join()
                assertEquals(setOf("P:a"), repository.getSubagentModelIds())
                val reopened = SettingsManager(context)
                assertEquals(setOf("P:a"), reopened.subagentEnabledModels.first())
                assertNull(reopened.subagentRecommendedModel.first())
            } finally {
                scope.cancel()
            }
        }
    }

    private suspend fun restore(manager: SettingsManager, text: String, replace: Boolean = false) {
        PortableSettingsArchive.restoreFromJsonObject(
            Json.parseToJsonElement(text).jsonObject, manager, replace, false, null,
        ) { it }
    }

    private suspend fun withSettings(block: suspend (SettingsManager, Context) -> Unit) {
        val directory = Files.createTempDirectory("agora-subagent-model-settings").toFile()
        val context = mockk<Context>()
        every { context.applicationContext } returns context
        every { context.filesDir } returns directory
        // The production preferencesDataStore delegate is process-wide, not keyed by Context.
        val storageJob = SupervisorJob()
        val dataStore = PreferenceDataStoreFactory.create(
            scope = CoroutineScope(storageJob + Dispatchers.IO),
            migrations = listOf(modelProviderNamesMigration),
            storage = OkioStorage(FileSystem.SYSTEM, PreferencesSerializer) {
                directory.resolve("settings.preferences_pb").absolutePath.toPath()
            },
        )
        val settingsClass = "com.newoether.agora.data.SettingsManagerKt"
        mockkStatic(settingsClass)
        every { context.dataStore } returns dataStore
        mockkObject(SecretCrypto)
        every { SecretCrypto.encrypt(any()) } answers { firstArg<String>() }
        every { SecretCrypto.decrypt(any()) } answers { firstArg<String>() }
        try {
            block(SettingsManager(context), context)
        } finally {
            unmockkObject(SecretCrypto)
            unmockkStatic(settingsClass)
            storageJob.cancelAndJoin()
            directory.deleteRecursively()
        }
    }
}
