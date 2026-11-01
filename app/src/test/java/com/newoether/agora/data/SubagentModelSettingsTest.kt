package com.newoether.agora.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.emptyPreferences
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.coroutineScope
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.test.UnconfinedTestDispatcher
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import org.junit.Assert.*
import org.junit.Test

@OptIn(ExperimentalCoroutinesApi::class)
class SubagentModelSettingsTest {
    @Test fun unsetAndExplicitEmptySurviveReopenWithoutInheritingGlobalModels() = runTest {
        val data = MemoryDataStore()
        val store = SettingsModelPreferenceStore(data, Json)
        store.saveEnabledModels(setOf("P:a", "P:b"))
        assertNull(store.subagentEnabledModels.first())
        assertEquals(emptySet<String>(), store.subagentModelIds.first())
        store.saveSubagentRecommendedModel("P:a")
        assertNull(store.subagentRecommendedModel.first())
        assertNull(store.subagentEnabledModels.first())
        val unsetReopened = SettingsModelPreferenceStore(MemoryDataStore(data.data.first()), Json)
        assertNull(unsetReopened.subagentEnabledModels.first())
        assertEquals(emptySet<String>(), unsetReopened.subagentModelIds.first())
        store.saveSubagentEnabledModels(emptySet())
        val reopened = SettingsModelPreferenceStore(MemoryDataStore(data.data.first()), Json)
        assertEquals(emptySet<String>(), reopened.subagentEnabledModels.first())
        assertEquals(emptySet<String>(), reopened.subagentModelIds.first())
        assertNull(reopened.subagentRecommendedModel.first())
        assertEquals(setOf("P:a", "P:b"), reopened.enabledModels.first())
        reopened.saveSubagentEnabledModels(null)
        assertNull(reopened.subagentEnabledModels.first())
        assertEquals(emptySet<String>(), reopened.subagentModelIds.first())
    }

    @Test fun firstAddIncludesOnlySelectedModelsAndPreservesExistingConfigurationOnReopen() = runTest {
        val data = MemoryDataStore()
        val store = SettingsModelPreferenceStore(data, Json)
        store.saveEnabledModels(setOf("P:a", "P:b", "P:c"))
        store.addSubagentModels(setOf("P:b"))
        assertEquals(setOf("P:b"), store.subagentEnabledModels.first())
        val reopened = SettingsModelPreferenceStore(MemoryDataStore(data.data.first()), Json)
        assertEquals(setOf("P:b"), reopened.subagentEnabledModels.first())
        assertEquals(setOf("P:b"), reopened.subagentModelIds.first())
        reopened.addSubagentModels(setOf("P:c"))
        assertEquals(setOf("P:b", "P:c"), reopened.subagentEnabledModels.first())
        assertEquals(setOf("P:a", "P:b", "P:c"), reopened.enabledModels.first())
        store.saveSubagentEnabledModels(null)
        store.removeSubagentModel("P:a")
        assertEquals(emptySet<String>(), store.subagentEnabledModels.first())
        assertEquals(emptySet<String>(), store.subagentModelIds.first())
    }

    @Test fun removingConfiguredCandidateClearsRecommendationInOneEmission() = runTest {
        val data = MemoryDataStore()
        val store = SettingsModelPreferenceStore(data, Json)
        store.saveEnabledModels(setOf("P:a", "P:b"))
        store.saveSubagentEnabledModels(setOf("P:a", "P:b"))
        store.saveSubagentRecommendedModel("P:a")
        val observed = mutableListOf<Preferences>()
        backgroundScope.launch(UnconfinedTestDispatcher(testScheduler)) { data.data.toList(observed) }
        store.removeSubagentModel("P:a")
        assertEquals(2, observed.size)
        assertEquals(setOf("P:b"), observed.last()[SUBAGENT_ENABLED_MODELS])
        assertNull(observed.last()[SUBAGENT_RECOMMENDED_MODEL])
        assertEquals(setOf("P:a", "P:b"), observed.last()[ENABLED_MODELS])
    }

    @Test fun concurrentAddRemoveReadsLatestPersistedSetInsteadOfLosingOtherChanges() = runTest {
        val store = SettingsModelPreferenceStore(MemoryDataStore(), Json)
        store.saveEnabledModels(setOf("P:a", "P:b", "P:c"))
        store.saveSubagentEnabledModels(emptySet())
        coroutineScope {
            launch { store.addSubagentModels(setOf("P:a")) }
            launch { store.addSubagentModels(setOf("P:b")) }
        }
        assertEquals(setOf("P:a", "P:b"), store.subagentEnabledModels.first())
        coroutineScope {
            launch { store.removeSubagentModel("P:a") }
            launch { store.addSubagentModels(setOf("P:c")) }
        }
        assertEquals(setOf("P:b", "P:c"), store.subagentEnabledModels.first())
        store.saveSubagentEnabledModels(null)
        store.addSubagentModels(setOf("P:a"))
        assertEquals(setOf("P:a"), store.subagentEnabledModels.first())
    }

    @Test fun staleAddIgnoresCurrentlyDisabledModelsAndCannotResurrectThemLater() = runTest {
        val store = SettingsModelPreferenceStore(MemoryDataStore(), Json)
        store.saveEnabledModels(setOf("P:a"))
        store.addSubagentModels(setOf("P:removed"))
        assertNull(store.subagentEnabledModels.first())
        store.saveSubagentEnabledModels(emptySet())
        store.addSubagentModels(setOf("P:a", "P:removed"))
        assertEquals(setOf("P:a"), store.subagentEnabledModels.first())
        store.saveEnabledModels(setOf("P:a", "P:removed"))
        assertEquals(setOf("P:a"), store.subagentModelIds.first())
    }

    @Test fun staleRecommendedSelectionIsANoOpAndNeverThrowsOrOverwritesAValidChoice() = runTest {
        val data = MemoryDataStore()
        val store = SettingsModelPreferenceStore(data, Json)
        store.saveEnabledModels(setOf("P:a", "P:b"))
        store.saveSubagentEnabledModels(setOf("P:a"))
        store.saveSubagentRecommendedModel("P:a")
        val before = data.data.first()
        assertTrue(runCatching { store.saveSubagentRecommendedModel("P:b") }.isSuccess)
        assertEquals(before, data.data.first())
        store.saveEnabledModels(setOf("P:b"))
        assertNull(store.subagentRecommendedModel.first())
        assertTrue(runCatching { store.saveSubagentRecommendedModel("P:a") }.isSuccess)
        assertNull(store.subagentRecommendedModel.first())
        store.saveSubagentRecommendedModel(null)
        assertNull(store.subagentRecommendedModel.first())
    }

    @Test fun mergeUsesOnlyConfiguredLocalSetsAndPreservesNullAndUnavailableArchiveRefs() = runTest {
        val store = SettingsModelPreferenceStore(MemoryDataStore(), Json)
        store.saveEnabledModels(setOf("P:a", "P:b"))
        store.saveSubagentEnabledModels(setOf("P:b"), merge = true)
        assertEquals(setOf("P:b"), store.subagentEnabledModels.first())
        store.saveSubagentRecommendedModel("P:b")
        store.saveSubagentEnabledModels(null, merge = true)
        assertEquals(setOf("P:b"), store.subagentEnabledModels.first())
        assertEquals("P:b", store.subagentRecommendedModel.first())
        store.saveSubagentEnabledModels(setOf("P:unavailable"), merge = true)
        assertEquals(setOf("P:b", "P:unavailable"), store.subagentEnabledModels.first())
        assertEquals(setOf("P:b"), store.subagentModelIds.first())
        store.saveSubagentEnabledModels(emptySet(), merge = true)
        assertEquals(setOf("P:b", "P:unavailable"), store.subagentEnabledModels.first())
        store.saveSubagentEnabledModels(null)
        store.saveSubagentEnabledModels(emptySet(), merge = true)
        assertEquals(emptySet<String>(), store.subagentEnabledModels.first())
    }

    @Test fun customModelRenameAndDeleteRemapBothRefsWithoutMaterializingUnsetList() = runTest {
        val data = MemoryDataStore()
        val store = SettingsModelPreferenceStore(data, Json)
        store.addCustomModel("P:old", "")
        store.saveSubagentRecommendedModel("P:old")
        store.replaceCustomModel("P:old", "P:middle", "")
        assertNull(store.subagentEnabledModels.first())
        assertNull(store.subagentRecommendedModel.first())
        store.saveSubagentEnabledModels(setOf("P:middle", "Other:keep"))
        store.saveSubagentRecommendedModel("P:middle")
        store.replaceCustomModel("P:middle", "P:new", "")
        assertEquals(setOf("P:new", "Other:keep"), store.subagentEnabledModels.first())
        assertEquals("P:new", store.subagentRecommendedModel.first())
        store.replaceCustomModel("P:new", null, "")
        assertEquals(setOf("Other:keep"), store.subagentEnabledModels.first())
        assertNull(store.subagentRecommendedModel.first())
    }

    @Test fun providerIdentityMigrationRemapsSubagentReferencesAndKeepsNullOrEmptyDistinct() = runTest {
        for (configured in listOf<Set<String>?>(null, emptySet(), setOf("Relay:a"))) {
            val data = MemoryDataStore()
            val store = SettingsModelPreferenceStore(data, Json)
            data.edit {
                it[CUSTOM_PROVIDERS_JSON] = Json.encodeToString(listOf(CustomProviderConfig("Relay")))
                it[ENABLED_MODELS] = setOf("Relay:a")
                if (configured != null) it[SUBAGENT_ENABLED_MODELS] = configured
                it[SUBAGENT_RECOMMENDED_MODEL] = "Relay:a"
            }
            store.normalizeCustomProviderIdentities()
            val model = store.customProviders.first().single().providerId + ":a"
            assertEquals(configured?.mapTo(linkedSetOf()) { model }, store.subagentEnabledModels.first())
            assertEquals(configured.orEmpty().mapTo(linkedSetOf()) { model }, store.subagentModelIds.first())
            assertEquals(model, store.subagentRecommendedModel.first())
            val reopened = SettingsModelPreferenceStore(MemoryDataStore(data.data.first()), Json)
            assertEquals(store.subagentEnabledModels.first(), reopened.subagentEnabledModels.first())
            assertEquals(model, reopened.subagentRecommendedModel.first())
        }
    }

    private class MemoryDataStore(initial: Preferences = emptyPreferences()) : DataStore<Preferences> {
        private val mutex = Mutex()
        private val state = MutableStateFlow(initial)
        override val data: Flow<Preferences> = state
        override suspend fun updateData(transform: suspend (Preferences) -> Preferences): Preferences =
            mutex.withLock { transform(state.value).also { state.value = it } }
    }
}
