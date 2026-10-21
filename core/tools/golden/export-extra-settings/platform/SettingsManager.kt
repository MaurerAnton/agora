// Storage boundary for ExportExtraSettings.restoreLegacyFromJsonObject.
//
// The real SettingsManager is a DataStore-backed Android class. The restore path only calls the
// save methods listed below and reads the modelAliases/customModels flows, so this double records
// the exact arguments in call order and exposes the two flows. No policy logic lives here: the
// save methods do not coerce, normalise or blank-filter, because that is SettingsManager's job and
// the trace pins the values the policy passes to it.
package com.newoether.agora.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.MutableStateFlow

sealed interface SavedValue

data class SavedBoolean(val value: Boolean) : SavedValue
data class SavedInt(val value: Int) : SavedValue
data class SavedFloat(val value: Float) : SavedValue
data class SavedString(val value: String) : SavedValue
data class SavedStringSet(val value: Set<String>) : SavedValue
data class SavedStringMap(val value: Map<String, String>) : SavedValue
data class SavedConversation(
    val conversationId: String,
    val settings: ConversationSettings,
) : SavedValue

data class RecordedCall(val name: String, val value: SavedValue)

class SettingsManager(
    modelAliasesSeed: Map<String, String> = emptyMap(),
    customModelsSeed: Set<String> = emptySet(),
) {
    private val modelAliasesState = MutableStateFlow(modelAliasesSeed)
    private val customModelsState = MutableStateFlow(customModelsSeed)

    val modelAliases: Flow<Map<String, String>> = modelAliasesState
    val customModels: Flow<Set<String>> = customModelsState

    val calls = mutableListOf<RecordedCall>()

    private fun record(name: String, value: SavedValue) {
        calls += RecordedCall(name, value)
    }

    suspend fun saveImageTranscriptionEnabled(enabled: Boolean) =
        record("saveImageTranscriptionEnabled", SavedBoolean(enabled))

    suspend fun saveImageTranscriptionEnabledModels(models: Set<String>) =
        record("saveImageTranscriptionEnabledModels", SavedStringSet(models))

    suspend fun saveImageTranscriptionModel(model: String?) =
        record("saveImageTranscriptionModel", SavedString(model ?: "<null>"))

    suspend fun saveImageTranscriptionBatchSize(size: Int) =
        record("saveImageTranscriptionBatchSize", SavedInt(size))

    suspend fun saveImageTranscriptionPrompt(prompt: String) =
        record("saveImageTranscriptionPrompt", SavedString(prompt))

    suspend fun saveWebSearchNumResults(n: Int) =
        record("saveWebSearchNumResults", SavedInt(n))

    suspend fun saveSearchContextWindow(n: Int) =
        record("saveSearchContextWindow", SavedInt(n))

    suspend fun saveSearchMatchLimit(n: Int) =
        record("saveSearchMatchLimit", SavedInt(n))

    suspend fun saveDefaultTemperature(value: Float?) =
        record("saveDefaultTemperature", SavedFloat(value!!))

    suspend fun saveDefaultMaxTokens(value: Int?) =
        record("saveDefaultMaxTokens", SavedInt(value!!))

    suspend fun saveDefaultTopP(value: Float?) =
        record("saveDefaultTopP", SavedFloat(value!!))

    suspend fun saveDefaultFrequencyPenalty(value: Float?) =
        record("saveDefaultFrequencyPenalty", SavedFloat(value!!))

    suspend fun saveDefaultPresencePenalty(value: Float?) =
        record("saveDefaultPresencePenalty", SavedFloat(value!!))

    suspend fun saveConversationSettings(
        conversationId: String,
        settings: ConversationSettings,
    ): Map<String, ConversationSettings> {
        record("saveConversationSettings", SavedConversation(conversationId, settings))
        return emptyMap()
    }

    suspend fun saveShowDocumentationFab(enabled: Boolean) =
        record("saveShowDocumentationFab", SavedBoolean(enabled))

    suspend fun saveProxyEnabled(enabled: Boolean) =
        record("saveProxyEnabled", SavedBoolean(enabled))

    suspend fun saveProxyType(type: String) =
        record("saveProxyType", SavedString(type))

    suspend fun saveProxyHost(host: String) =
        record("saveProxyHost", SavedString(host))

    suspend fun saveProxyPort(port: String) =
        record("saveProxyPort", SavedString(port))

    suspend fun saveProxyUsername(user: String) =
        record("saveProxyUsername", SavedString(user))

    suspend fun saveProxyPassword(pass: String) =
        record("saveProxyPassword", SavedString(pass))

    suspend fun saveProxyBypass(bypass: String) =
        record("saveProxyBypass", SavedString(bypass))

    suspend fun saveThemeMode(mode: String) =
        record("saveThemeMode", SavedString(mode))

    suspend fun saveColorScheme(scheme: String) =
        record("saveColorScheme", SavedString(scheme))

    suspend fun saveDynamicColor(enabled: Boolean) =
        record("saveDynamicColor", SavedBoolean(enabled))

    suspend fun saveBlurEffectsEnabled(enabled: Boolean) =
        record("saveBlurEffectsEnabled", SavedBoolean(enabled))

    suspend fun saveReduceMotion(enabled: Boolean) =
        record("saveReduceMotion", SavedBoolean(enabled))

    suspend fun saveOpenAiServiceTierEnabled(enabled: Boolean) =
        record("saveOpenAiServiceTierEnabled", SavedBoolean(enabled))

    suspend fun saveOpenAiServiceTier(tier: String) =
        record("saveOpenAiServiceTier", SavedString(tier))

    suspend fun saveHapticsEnabled(enabled: Boolean) =
        record("saveHapticsEnabled", SavedBoolean(enabled))

    suspend fun saveDetailedTokenUsage(enabled: Boolean) =
        record("saveDetailedTokenUsage", SavedBoolean(enabled))

    suspend fun saveAutoExpandActiveGroup(enabled: Boolean) =
        record("saveAutoExpandActiveGroup", SavedBoolean(enabled))

    suspend fun saveSchemeStyle(style: String) =
        record("saveSchemeStyle", SavedString(style))

    suspend fun saveFontPreference(value: String) =
        record("saveFontPreference", SavedString(value))

    suspend fun saveAutoUpdateCheck(enabled: Boolean) =
        record("saveAutoUpdateCheck", SavedBoolean(enabled))

    suspend fun saveAutomationToolsEnabled(enabled: Boolean) =
        record("saveAutomationToolsEnabled", SavedBoolean(enabled))

    suspend fun saveExactExecutionEnabled(enabled: Boolean) =
        record("saveExactExecutionEnabled", SavedBoolean(enabled))

    suspend fun saveAutomationWakeLockEnabled(enabled: Boolean) =
        record("saveAutomationWakeLockEnabled", SavedBoolean(enabled))

    suspend fun saveModelAliases(aliases: Map<String, String>) =
        record("saveModelAliases", SavedStringMap(aliases))

    suspend fun saveCustomModels(models: Set<String>) =
        record("saveCustomModels", SavedStringSet(models))
}
