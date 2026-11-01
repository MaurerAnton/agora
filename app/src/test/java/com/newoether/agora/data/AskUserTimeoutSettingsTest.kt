package com.newoether.agora.data

import android.content.Context
import androidx.datastore.core.okio.OkioStorage
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.PreferencesSerializer
import androidx.datastore.preferences.core.edit
import com.newoether.agora.util.SecretCrypto
import io.mockk.every
import io.mockk.mockk
import io.mockk.mockkObject
import io.mockk.mockkStatic
import io.mockk.unmockkObject
import io.mockk.unmockkStatic
import java.nio.file.Files
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import okio.FileSystem
import okio.Path.Companion.toPath
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class AskUserTimeoutSettingsTest {
    @Test
    fun timeoutUsesSevenClosedPresetsAndDefaultsEveryUnsupportedIntegerToFive() {
        assertEquals(5, DEFAULT_ASK_USER_TIMEOUT_MINUTES)
        assertEquals(listOf(1, 2, 5, 10, 15, 30, 60), ASK_USER_TIMEOUT_PRESETS)
        ASK_USER_TIMEOUT_PRESETS.forEach { minutes ->
            assertEquals(minutes, normalizeAskUserTimeoutMinutes(minutes))
        }
        listOf(null, Int.MIN_VALUE, -1, 0, 3, 4, 6, 14, 29, 59, 61, Int.MAX_VALUE).forEach { minutes ->
            assertEquals("Unsupported timeout $minutes", 5, normalizeAskUserTimeoutMinutes(minutes))
        }
        assertEquals("ask_user_enabled", ASK_USER_ENABLED.name)
        assertEquals("ask_user_timeout_minutes", ASK_USER_TIMEOUT_MINUTES.name)
    }

    @Test
    fun freshDefaultsAndEverySavedPresetSurviveReloadAndExportAsNumbers() = runTest {
        withSettings { manager, context ->
            assertTrue(manager.askUserEnabled.first())
            assertEquals(5, manager.askUserTimeoutMinutes.first())
            var exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonPrimitive(true), exported["askUserEnabled"])
            assertEquals(JsonPrimitive(5), exported["askUserTimeoutMinutes"])
            manager.saveAskUserEnabled(false)
            for (minutes in listOf(1, 2, 5, 10, 15, 30, 60)) {
                manager.saveAskUserTimeoutMinutes(minutes)
                assertEquals(minutes, context.dataStore.data.first()[ASK_USER_TIMEOUT_MINUTES])
                val reloaded = SettingsManager(context)
                assertEquals(minutes, reloaded.askUserTimeoutMinutes.first())
                assertFalse(reloaded.askUserEnabled.first())
                exported = PortableSettingsArchive.toJsonObject(reloaded, customFontIncluded = false)
                assertEquals(JsonPrimitive(minutes), exported["askUserTimeoutMinutes"])
                assertFalse((exported["askUserTimeoutMinutes"] as JsonPrimitive).isString)
                assertEquals(JsonPrimitive(false), exported["askUserEnabled"])
                assertFalse(exported.containsKey(ASK_USER_TIMEOUT_MINUTES.name))
            }
        }
    }

    @Test
    fun invalidStoredReadsAndExportUseFiveWithoutRepairingStorageUntilSaved() = runTest {
        withSettings { manager, context ->
            manager.saveAskUserEnabled(false)
            for (invalid in listOf(Int.MIN_VALUE, 0, 3, 7, 59, 61, Int.MAX_VALUE)) {
                context.dataStore.edit { it[ASK_USER_TIMEOUT_MINUTES] = invalid }
                val before = context.dataStore.data.first().asMap()
                assertEquals(5, manager.askUserTimeoutMinutes.first())
                assertEquals(5, SettingsManager(context).askUserTimeoutMinutes.first())
                assertEquals(
                    JsonPrimitive(5),
                    PortableSettingsArchive.toJsonObject(manager, false)["askUserTimeoutMinutes"],
                )
                assertEquals(before, context.dataStore.data.first().asMap())
                manager.saveAskUserTimeoutMinutes(invalid)
                assertEquals(5, context.dataStore.data.first()[ASK_USER_TIMEOUT_MINUTES])
                assertFalse(manager.askUserEnabled.first())
            }
        }
    }

    @Test
    fun numericArchiveRoundTripRestoresBothFieldsAndNormalizesUnsupportedImports() = runTest {
        withSettings { manager, context ->
            manager.saveAskUserEnabled(false)
            manager.saveAskUserTimeoutMinutes(60)
            val exported = PortableSettingsArchive.toJsonObject(manager, false)
            manager.saveAskUserEnabled(true)
            manager.saveAskUserTimeoutMinutes(1)
            restore(manager, exported, replace = true)
            assertFalse(SettingsManager(context).askUserEnabled.first())
            assertEquals(60, SettingsManager(context).askUserTimeoutMinutes.first())
            for (minutes in listOf(1, 2, 5, 10, 15, 30, 60)) {
                restore(manager, JsonObject(mapOf("askUserTimeoutMinutes" to JsonPrimitive(minutes))))
                assertEquals(minutes, manager.askUserTimeoutMinutes.first())
            }
            for (invalid in listOf(-1, 0, 3, 59, 61, Int.MAX_VALUE)) {
                restore(manager, JsonObject(mapOf("askUserTimeoutMinutes" to JsonPrimitive(invalid))))
                assertEquals(5, manager.askUserTimeoutMinutes.first())
                assertEquals(5, context.dataStore.data.first()[ASK_USER_TIMEOUT_MINUTES])
                assertFalse(manager.askUserEnabled.first())
            }
        }
    }

    @Test
    fun mergeOfOlderOrPartialArchivesPreservesEachAbsentInteractionField() = runTest {
        withSettings { manager, context ->
            manager.saveAskUserEnabled(false)
            manager.saveAskUserTimeoutMinutes(30)
            val before = context.dataStore.data.first()
            restore(manager, JsonObject(emptyMap()))
            assertEquals(before[ASK_USER_ENABLED], context.dataStore.data.first()[ASK_USER_ENABLED])
            assertEquals(before[ASK_USER_TIMEOUT_MINUTES], context.dataStore.data.first()[ASK_USER_TIMEOUT_MINUTES])
            assertFalse(manager.askUserEnabled.first())
            assertEquals(30, manager.askUserTimeoutMinutes.first())
            restore(manager, JsonObject(mapOf("askUserEnabled" to JsonPrimitive(true))))
            assertTrue(manager.askUserEnabled.first())
            assertEquals(30, manager.askUserTimeoutMinutes.first())
            restore(manager, JsonObject(mapOf("askUserTimeoutMinutes" to JsonPrimitive(2))))
            assertTrue(manager.askUserEnabled.first())
            assertEquals(2, manager.askUserTimeoutMinutes.first())
            restore(manager, JsonObject(mapOf("askUserEnabled" to JsonPrimitive(false))))
            assertFalse(SettingsManager(context).askUserEnabled.first())
            assertEquals(2, SettingsManager(context).askUserTimeoutMinutes.first())
        }
    }

    @Test
    fun replaceOfOlderArchiveClearsBothKeysAndKeepsDeviceLocalPreferencesAndSecrets() = runTest {
        withSettings { manager, context ->
            manager.saveAskUserEnabled(false)
            manager.saveAskUserTimeoutMinutes(60)
            context.dataStore.edit {
                it[LOCAL_MODEL_IDLE_RETENTION_MINUTES] = 15
                it[LOCAL_LOW_CONTEXT_MODE_ENABLED] = true
                it[SANDBOX_ENABLED] = true
                it[AUTO_BACKUP_ENABLED] = true
                it[PROXY_PASSWORD] = "device-secret"
            }
            val localKeys = setOf(
                LOCAL_MODEL_IDLE_RETENTION_MINUTES, LOCAL_LOW_CONTEXT_MODE_ENABLED,
                SANDBOX_ENABLED, AUTO_BACKUP_ENABLED, PROXY_PASSWORD,
            )
            val localValues = context.dataStore.data.first().asMap().filterKeys { it in localKeys }
            restore(manager, JsonObject(emptyMap()), replace = true)
            val preferences = context.dataStore.data.first()
            assertNull(preferences[ASK_USER_ENABLED])
            assertNull(preferences[ASK_USER_TIMEOUT_MINUTES])
            assertTrue(SettingsManager(context).askUserEnabled.first())
            assertEquals(5, SettingsManager(context).askUserTimeoutMinutes.first())
            assertEquals(localValues, preferences.asMap().filterKeys { it in localKeys })
        }
    }

    @Test
    fun replaceOfPartialArchiveRestoresPresentFieldAndDefaultsOnlyTheMissingOne() = runTest {
        withSettings { manager, context ->
            manager.saveAskUserEnabled(false)
            manager.saveAskUserTimeoutMinutes(60)
            restore(manager, JsonObject(mapOf("askUserTimeoutMinutes" to JsonPrimitive(15))), replace = true)
            assertTrue(manager.askUserEnabled.first())
            assertEquals(15, manager.askUserTimeoutMinutes.first())
            assertNull(context.dataStore.data.first()[ASK_USER_ENABLED])
            restore(manager, JsonObject(mapOf("askUserEnabled" to JsonPrimitive(false))), replace = true)
            assertFalse(SettingsManager(context).askUserEnabled.first())
            assertEquals(5, SettingsManager(context).askUserTimeoutMinutes.first())
            assertNull(context.dataStore.data.first()[ASK_USER_TIMEOUT_MINUTES])
        }
    }

    private suspend fun restore(manager: SettingsManager, archive: JsonObject, replace: Boolean = false) {
        PortableSettingsArchive.restoreFromJsonObject(archive, manager, replace, false, null) { it }
    }

    private suspend fun withSettings(block: suspend (SettingsManager, Context) -> Unit) {
        val directory = Files.createTempDirectory("agora-ask-user-timeout").toFile()
        val context = mockk<Context>()
        every { context.applicationContext } returns context
        every { context.filesDir } returns directory
        // The production delegate is process-wide, so each test injects its own real file store.
        val storageJob = SupervisorJob()
        val store = PreferenceDataStoreFactory.create(
            scope = CoroutineScope(storageJob + Dispatchers.IO),
            migrations = listOf(modelProviderNamesMigration),
            storage = OkioStorage(FileSystem.SYSTEM, PreferencesSerializer) {
                directory.resolve("settings.preferences_pb").absolutePath.toPath()
            },
        )
        val settingsClass = "com.newoether.agora.data.SettingsManagerKt"
        mockkStatic(settingsClass)
        every { context.dataStore } returns store
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
