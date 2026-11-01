package com.newoether.agora.data

import android.content.Context
import androidx.datastore.core.DataStore
import androidx.datastore.core.okio.OkioStorage
import androidx.datastore.preferences.core.PreferenceDataStoreFactory
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.PreferencesSerializer
import androidx.datastore.preferences.core.edit
import com.newoether.agora.data.repository.SettingsRepository
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
import kotlinx.coroutines.async
import kotlinx.coroutines.awaitAll
import kotlinx.coroutines.cancel
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.jsonObject
import okio.FileSystem
import okio.Path.Companion.toPath
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class SubagentLimitsSettingsTest {
    @Test
    fun defaultsAndExplicitIntegersAlwaysResolveToSupportedPositiveLimits() {
        assertEquals(16, normalizeSubagentMaxTotal(null))
        assertEquals(4, normalizeSubagentMaxConcurrent(null, 16))
        assertEquals(1, normalizeSubagentMaxConcurrent(null, 1))
        for (value in listOf(Int.MIN_VALUE, -1, 0)) {
            assertEquals(1, normalizeSubagentMaxTotal(value))
            assertEquals(1, normalizeSubagentMaxConcurrent(value, 64))
        }
        for (value in listOf(17, 32, 64, 65, Int.MAX_VALUE)) {
            assertEquals(16, normalizeSubagentMaxTotal(value))
            assertEquals(16, normalizeSubagentMaxConcurrent(value, 64))
            assertEquals(2, normalizeSubagentMaxConcurrent(value, 2))
        }
        for (total in 1..16) {
            assertEquals(total, normalizeSubagentMaxTotal(total))
            for (concurrent in 1..16) {
                assertEquals(concurrent.coerceAtMost(total), normalizeSubagentMaxConcurrent(concurrent, total))
            }
        }
    }

    @Test
    fun savesBothKeysInOneTransactionAndLoweringTotalRetainsUnrelatedSettings() = runTest {
        withSettings { manager, context, store ->
            assertLimits(manager, 16, 4)
            manager.saveSubagentToolsEnabled(false)
            manager.saveEnabledModels(setOf("P:model"))
            manager.saveSubagentEnabledModels(setOf("P:model"))
            manager.saveSubagentRecommendedModel("P:model")
            manager.saveSubagentCapsuleDisplayMode("grouped")
            manager.saveLocalLowContextModeEnabled(true)
            val unrelated = store.data.first().asMap()
                .filterKeys { it != SUBAGENT_MAX_TOTAL && it != SUBAGENT_MAX_CONCURRENT }

            var writes = store.updateCount
            manager.saveSubagentMaxConcurrent(99)
            assertEquals(writes + 1, store.updateCount)
            assertStoredLimits(store, 16, 16)
            writes = store.updateCount
            manager.saveSubagentMaxTotal(2)
            assertEquals(writes + 1, store.updateCount)
            assertStoredLimits(store, 2, 2)
            assertEquals(listOf(16 to 16, 2 to 2), store.limitChanges)
            assertEquals(
                unrelated,
                store.data.first().asMap().filterKeys { it != SUBAGENT_MAX_TOTAL && it != SUBAGENT_MAX_CONCURRENT },
            )
            assertLimits(SettingsManager(context), 2, 2)

            manager.saveSubagentMaxTotal(Int.MAX_VALUE)
            assertStoredLimits(store, 16, 2)
            manager.saveSubagentMaxConcurrent(Int.MIN_VALUE)
            assertStoredLimits(store, 16, 1)
            manager.saveSubagentMaxTotal(0)
            assertStoredLimits(store, 1, 1)
        }
    }

    @Test
    fun storedOutOfRangeValuesAreNormalizedWithoutChangingStorageOnRead() = runTest {
        withSettings { manager, _, store ->
            store.edit {
                it[SUBAGENT_MAX_TOTAL] = 0
                it[SUBAGENT_MAX_CONCURRENT] = 99
            }
            var writes = store.updateCount
            assertLimits(manager, 1, 1)
            var exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonPrimitive(1), exported["subagentMaxTotal"])
            assertEquals(JsonPrimitive(1), exported["subagentMaxConcurrent"])
            assertEquals(writes, store.updateCount)
            assertStoredLimits(store, 0, 99)

            store.edit {
                it[SUBAGENT_MAX_TOTAL] = 64
                it[SUBAGENT_MAX_CONCURRENT] = 64
            }
            writes = store.updateCount
            assertLimits(manager, 16, 16)
            exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonPrimitive(16), exported["subagentMaxTotal"])
            assertEquals(JsonPrimitive(16), exported["subagentMaxConcurrent"])
            assertEquals(writes, store.updateCount)
            assertStoredLimits(store, 64, 64)
            manager.saveSubagentMaxTotal(64)
            assertStoredLimits(store, 16, 16)
        }
    }

    @Test
    fun archiveRoundTripRestoresBothIntegersTogetherAndClampsLegacyTotal() = runTest {
        withSettings { manager, context, store ->
            manager.saveSubagentLimits(16, 16)
            val exported = PortableSettingsArchive.toJsonObject(manager, false)
            assertEquals(JsonPrimitive(16), exported["subagentMaxTotal"])
            assertEquals(JsonPrimitive(16), exported["subagentMaxConcurrent"])
            assertFalse(exported.containsKey(SUBAGENT_MAX_TOTAL.name))
            assertFalse(exported.containsKey(SUBAGENT_MAX_CONCURRENT.name))
            manager.saveSubagentLimits(2, 2)
            val previousChanges = store.limitChanges.size
            restore(manager, """{"subagentMaxTotal":64,"subagentMaxConcurrent":16}""")
            assertEquals(listOf(16 to 16), store.limitChanges.drop(previousChanges))
            assertStoredLimits(store, 16, 16)

            manager.saveSubagentLimits(1, 1)
            PortableSettingsArchive.restoreFromJsonObject(exported, manager, true, false, null) { it }
            assertLimits(SettingsManager(context), 16, 16)
            assertStoredLimits(store, 16, 16)
        }
    }

    @Test
    fun mergePreservesAbsentLimitsAndNormalizesPresentValuesAgainstTheirCounterpart() = runTest {
        withSettings { manager, _, store ->
            manager.saveSubagentLimits(12, 8)
            val previousChanges = store.limitChanges.size
            restore(manager, "{}")
            assertEquals(previousChanges, store.limitChanges.size)
            assertStoredLimits(store, 12, 8)
            restore(manager, """{"subagentMaxTotal":3}""")
            assertStoredLimits(store, 3, 3)
            restore(manager, """{"subagentMaxConcurrent":16}""")
            assertStoredLimits(store, 3, 3)
            restore(manager, """{"subagentMaxTotal":64}""")
            assertStoredLimits(store, 16, 3)
            restore(manager, """{"subagentMaxTotal":999,"subagentMaxConcurrent":999}""")
            assertStoredLimits(store, 16, 16)
            restore(manager, """{"subagentMaxTotal":0,"subagentMaxConcurrent":-1}""")
            assertStoredLimits(store, 1, 1)
        }
    }

    @Test
    fun replaceClearsPortableLimitsAndKeepsDeviceLocalAndSecretKeys() = runTest {
        withSettings { manager, context, store ->
            manager.saveSubagentLimits(12, 12)
            store.edit {
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
            val localValues = store.data.first().asMap().filterKeys { it in localKeys }
            restore(manager, "{}", replace = true)
            assertNull(store.data.first()[SUBAGENT_MAX_TOTAL])
            assertNull(store.data.first()[SUBAGENT_MAX_CONCURRENT])
            assertLimits(SettingsManager(context), 16, 4)
            assertEquals(localValues, store.data.first().asMap().filterKeys { it in localKeys })

            restore(manager, """{"subagentMaxTotal":2}""", replace = true)
            assertStoredLimits(store, 2, 2)
            restore(manager, """{"subagentMaxConcurrent":12}""", replace = true)
            assertStoredLimits(store, 16, 12)
            assertEquals(localValues, store.data.first().asMap().filterKeys { it in localKeys })
        }
    }

    @Test
    fun simultaneousIndependentSavesKeepBothUserChangesAndEveryStoredPairValid() = runTest {
        withSettings { manager, _, store ->
            manager.saveSubagentLimits(8, 4)
            val previousChanges = store.limitChanges.size
            coroutineScope {
                listOf(
                    async(Dispatchers.Default) { manager.saveSubagentMaxTotal(16) },
                    async(Dispatchers.Default) { manager.saveSubagentMaxConcurrent(8) },
                ).awaitAll()
            }
            assertStoredLimits(store, 16, 8)
            assertEquals(2, store.limitChanges.size - previousChanges)
            coroutineScope {
                listOf(
                    async(Dispatchers.Default) { manager.saveSubagentMaxTotal(2) },
                    async(Dispatchers.Default) { manager.saveSubagentMaxConcurrent(16) },
                ).awaitAll()
            }
            assertStoredLimits(store, 2, 2)
            for ((total, concurrent) in store.limitChanges) {
                assertTrue(total != null && total in 1..16)
                assertTrue(concurrent != null && concurrent in 1..16)
                assertTrue(concurrent!! <= total!!)
            }
        }
    }

    @Test
    fun repositoryReadsFreshLimitsBeforeHotFlowsHaveLoadedAndPersistsActions() = runTest {
        withSettings { manager, _, store ->
            manager.saveSubagentLimits(3, 2)
            val scope = CoroutineScope(backgroundScope.coroutineContext)
            try {
                val repository = SettingsRepository(manager, scope)
                assertEquals(3, repository.getSubagentMaxTotal())
                assertEquals(2, repository.getSubagentMaxConcurrent())
                repository.awaitInitialLoad()
                assertEquals(3, repository.subagentMaxTotal.value)
                assertEquals(2, repository.subagentMaxConcurrent.value)
                repository.setSubagentMaxTotal(1).join()
                assertEquals(1, repository.getSubagentMaxTotal())
                assertEquals(1, repository.getSubagentMaxConcurrent())
                repository.setSubagentMaxConcurrent(16).join()
                assertStoredLimits(store, 1, 1)
            } finally {
                scope.cancel()
            }
        }
    }

    private suspend fun assertLimits(manager: SettingsManager, total: Int, concurrent: Int) {
        assertEquals(total, manager.subagentMaxTotal.first())
        assertEquals(concurrent, manager.subagentMaxConcurrent.first())
    }

    private suspend fun assertStoredLimits(store: DataStore<Preferences>, total: Int, concurrent: Int) {
        val preferences = store.data.first()
        assertEquals(total, preferences[SUBAGENT_MAX_TOTAL])
        assertEquals(concurrent, preferences[SUBAGENT_MAX_CONCURRENT])
    }

    private suspend fun restore(manager: SettingsManager, text: String, replace: Boolean = false) {
        PortableSettingsArchive.restoreFromJsonObject(
            Json.parseToJsonElement(text).jsonObject, manager, replace, false, null,
        ) { it }
    }

    private suspend fun withSettings(
        block: suspend (SettingsManager, Context, RecordingDataStore) -> Unit,
    ) {
        val directory = Files.createTempDirectory("agora-subagent-limits").toFile()
        val context = mockk<Context>()
        every { context.applicationContext } returns context
        every { context.filesDir } returns directory
        val storageJob = SupervisorJob()
        val store = RecordingDataStore(PreferenceDataStoreFactory.create(
            scope = CoroutineScope(storageJob + Dispatchers.IO),
            migrations = listOf(modelProviderNamesMigration),
            storage = OkioStorage(FileSystem.SYSTEM, PreferencesSerializer) {
                directory.resolve("settings.preferences_pb").absolutePath.toPath()
            },
        ))
        val settingsClass = "com.newoether.agora.data.SettingsManagerKt"
        mockkStatic(settingsClass)
        every { context.dataStore } returns store
        mockkObject(SecretCrypto)
        every { SecretCrypto.encrypt(any()) } answers { firstArg<String>() }
        every { SecretCrypto.decrypt(any()) } answers { firstArg<String>() }
        try {
            block(SettingsManager(context), context, store)
        } finally {
            unmockkObject(SecretCrypto)
            unmockkStatic(settingsClass)
            storageJob.cancelAndJoin()
            directory.deleteRecursively()
        }
    }

    /** Records actual DataStore transaction results, including any intermediate limit pair. */
    private class RecordingDataStore(private val delegate: DataStore<Preferences>) : DataStore<Preferences> by delegate {
        var updateCount = 0
            private set
        val limitChanges = mutableListOf<Pair<Int?, Int?>>()

        override suspend fun updateData(transform: suspend (Preferences) -> Preferences): Preferences =
            delegate.updateData { before ->
                val after = transform(before)
                updateCount++
                val previous = before[SUBAGENT_MAX_TOTAL] to before[SUBAGENT_MAX_CONCURRENT]
                val current = after[SUBAGENT_MAX_TOTAL] to after[SUBAGENT_MAX_CONCURRENT]
                if (previous != current) limitChanges += current
                after
            }
    }
}
