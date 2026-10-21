// Storage boundary for PortableSettingsArchive (both halves).
//
// The real SettingsManager is a DataStore-backed Android class. The archive reads exactly the 97
// flows below and, on restore, calls exactly the save methods below. This double:
//   * serves every flow from a scenario-seeded snapshot (the scenario must provide every key, so
//     no default is quietly reimplemented here);
//   * records each save as JSON with the exact argument the archive passed — no clamping,
//     blank-filtering or normalisation, because that belongs to SettingsManager, not the archive.
// No policy logic lives in this file.
package com.newoether.agora.data

import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flowOf
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

class SettingsManager(
    strings: Map<String, String> = emptyMap(),
    nullableStrings: Map<String, String?> = emptyMap(),
    booleans: Map<String, Boolean> = emptyMap(),
    ints: Map<String, Int> = emptyMap(),
    nullableInts: Map<String, Int?> = emptyMap(),
    floats: Map<String, Float> = emptyMap(),
    nullableFloats: Map<String, Float?> = emptyMap(),
    stringSets: Map<String, Set<String>> = emptyMap(),
    stringMaps: Map<String, Map<String, String>> = emptyMap(),
    boolMaps: Map<String, Map<String, Boolean>> = emptyMap(),
    embeddingModels: List<EmbeddingModelConfig> = emptyList(),
    shellDevices: List<ShellDeviceConfig> = emptyList(),
    mcpServers: List<McpServerConfig> = emptyList(),
    customProviders: List<CustomProviderConfig> = emptyList(),
) {
    private val stringValues = strings
    private val nullableStringValues = nullableStrings
    private val booleanValues = booleans
    private val intValues = ints
    private val nullableIntValues = nullableInts
    private val floatValues = floats
    private val nullableFloatValues = nullableFloats
    private val stringSetValues = stringSets
    private val stringMapValues = stringMaps
    private val boolMapValues = boolMaps
    private val embeddingModelValues = embeddingModels
    private val shellDeviceValues = shellDevices
    private val mcpServerValues = mcpServers
    private val customProviderValues = customProviders

    val calls = mutableListOf<JsonObject>()

    // ------------------------------------------------------------------ flows

    private fun string(key: String): Flow<String> = flowOf(stringValues.getValue(key))
    private fun nullableString(key: String): Flow<String?> = flowOf(nullableStringValues.getValue(key))
    private fun bool(key: String): Flow<Boolean> = flowOf(booleanValues.getValue(key))
    private fun int(key: String): Flow<Int> = flowOf(intValues.getValue(key))
    private fun nullableInt(key: String): Flow<Int?> = flowOf(nullableIntValues.getValue(key))
    private fun float(key: String): Flow<Float> = flowOf(floatValues.getValue(key))
    private fun nullableFloat(key: String): Flow<Float?> = flowOf(nullableFloatValues.getValue(key))
    private fun stringSet(key: String): Flow<Set<String>> = flowOf(stringSetValues.getValue(key))
    private fun stringMap(key: String): Flow<Map<String, String>> = flowOf(stringMapValues.getValue(key))
    private fun boolMap(key: String): Flow<Map<String, Boolean>> = flowOf(boolMapValues.getValue(key))

    val selectedModel: Flow<String> get() = string("selectedModel")
    val customModels: Flow<Set<String>> get() = stringSet("customModels")
    val enabledModels: Flow<Set<String>> get() = stringSet("enabledModels")
    val modelAliases: Flow<Map<String, String>> get() = stringMap("modelAliases")
    val modelProviderNames: Flow<Map<String, Boolean>> get() = boolMap("modelProviderNames")
    val maxContextWindow: Flow<Int> get() = int("maxContextWindow")
    val visualizeContextRollout: Flow<Boolean> get() = bool("visualizeContextRollout")
    val contextCompactEnabled: Flow<Boolean> get() = bool("contextCompactEnabled")
    val contextCompactModel: Flow<String?> get() = nullableString("contextCompactModel")
    val contextCompactPrompt: Flow<String> get() = string("contextCompactPrompt")
    val contextCompactRetainCount: Flow<Int> get() = int("contextCompactRetainCount")
    val contextCompactPreserveSystemPrompt: Flow<Boolean> get() =
        bool("contextCompactPreserveSystemPrompt")
    val contextCompactThresholdPercent: Flow<Int> get() = int("contextCompactThresholdPercent")
    val codeExecutionEnabled: Flow<Boolean> get() = bool("codeExecutionEnabled")
    val googleSearchEnabled: Flow<Boolean> get() = bool("googleSearchEnabled")
    val thinkingEnabled: Flow<Boolean> get() = bool("thinkingEnabled")
    val thinkingLevel: Flow<String> get() = string("thinkingLevel")
    val thinkingBudgetEnabled: Flow<Boolean> get() = bool("thinkingBudgetEnabled")
    val thinkingBudgetTokens: Flow<Int> get() = int("thinkingBudgetTokens")
    val openAiServiceTierEnabled: Flow<Boolean> get() = bool("openAiServiceTierEnabled")
    val openAiServiceTier: Flow<String> get() = string("openAiServiceTier")
    val openAiResponsesApiEnabled: Flow<Boolean> get() = bool("openAiResponsesApiEnabled")
    val anthropicCacheEnabled: Flow<Boolean> get() = bool("anthropicCacheEnabled")
    val anthropicCacheTtl: Flow<String> get() = string("anthropicCacheTtl")
    val providerBaseUrls: Flow<Map<String, String>> get() = stringMap("providerBaseUrls")
    val titleGenerationEnabled: Flow<Boolean> get() = bool("titleGenerationEnabled")
    val titleGenerationModel: Flow<String?> get() = nullableString("titleGenerationModel")
    val titleGenerationPrompt: Flow<String> get() = string("titleGenerationPrompt")
    val titleGenerationNotificationsEnabled: Flow<Boolean> get() =
        bool("titleGenerationNotificationsEnabled")
    val accessPastConversations: Flow<Boolean> get() = bool("accessPastConversations")
    val accessSavedMemories: Flow<Boolean> get() = bool("accessSavedMemories")
    val accessActiveMemory: Flow<Boolean> get() = bool("accessActiveMemory")
    val accessSkills: Flow<Boolean> get() = bool("accessSkills")
    val ragSearchEnabled: Flow<Boolean> get() = bool("ragSearchEnabled")
    val modelSearchMethod: Flow<String> get() = string("modelSearchMethod")
    val manualSearchMethod: Flow<String> get() = string("manualSearchMethod")
    val embeddingModels: Flow<List<EmbeddingModelConfig>> get() = flowOf(embeddingModelValues)
    val activeEmbeddingModelId: Flow<String> get() = string("activeEmbeddingModelId")
    val appLanguage: Flow<String> get() = string("appLanguage")
    val webSearchEnabled: Flow<Boolean> get() = bool("webSearchEnabled")
    val webSearchProvider: Flow<String> get() = string("webSearchProvider")
    val webSearchNumResults: Flow<Int> get() = int("webSearchNumResults")
    val webSearchBaseUrl: Flow<String> get() = string("webSearchBaseUrl")
    val imageGenEnabled: Flow<Boolean> get() = bool("imageGenEnabled")
    val imageGenModel: Flow<String?> get() = nullableString("imageGenModel")
    val imageGenSize: Flow<String> get() = string("imageGenSize")
    val searchContextWindow: Flow<Int> get() = int("searchContextWindow")
    val searchMatchLimit: Flow<Int> get() = int("searchMatchLimit")
    val ragThreshold: Flow<Float> get() = float("ragThreshold")
    val autoCacheEnabled: Flow<Boolean> get() = bool("autoCacheEnabled")
    val showUncachedNotification: Flow<Boolean> get() = bool("showUncachedNotification")
    val autoUpdateCheck: Flow<Boolean> get() = bool("autoUpdateCheck")
    val imageTranscriptionEnabled: Flow<Boolean> get() = bool("imageTranscriptionEnabled")
    val imageTranscriptionEnabledModels: Flow<Set<String>> get() =
        stringSet("imageTranscriptionEnabledModels")
    val imageTranscriptionModel: Flow<String?> get() = nullableString("imageTranscriptionModel")
    val imageTranscriptionBatchSize: Flow<Int> get() = int("imageTranscriptionBatchSize")
    val imageTranscriptionPrompt: Flow<String> get() = string("imageTranscriptionPrompt")
    val shellEnabled: Flow<Boolean> get() = bool("shellEnabled")
    val shellConfirmEnabled: Flow<Boolean> get() = bool("shellConfirmEnabled")
    val askUserEnabled: Flow<Boolean> get() = bool("askUserEnabled")
    val shellDevices: Flow<List<ShellDeviceConfig>> get() = flowOf(shellDeviceValues)
    val automationToolsEnabled: Flow<Boolean> get() = bool("automationToolsEnabled")
    val exactExecutionEnabled: Flow<Boolean> get() = bool("exactExecutionEnabled")
    val automationWakeLockEnabled: Flow<Boolean> get() = bool("automationWakeLockEnabled")
    val customProviders: Flow<List<CustomProviderConfig>> get() = flowOf(customProviderValues)
    val mcpServers: Flow<List<McpServerConfig>> get() = flowOf(mcpServerValues)
    val proxyEnabled: Flow<Boolean> get() = bool("proxyEnabled")
    val proxyType: Flow<String> get() = string("proxyType")
    val proxyHost: Flow<String> get() = string("proxyHost")
    val proxyPort: Flow<String> get() = string("proxyPort")
    val proxyUsername: Flow<String> get() = string("proxyUsername")
    val proxyBypass: Flow<String> get() = string("proxyBypass")
    val showDocumentationFab: Flow<Boolean> get() = bool("showDocumentationFab")
    val themeMode: Flow<String> get() = string("themeMode")
    val amoledEnabled: Flow<Boolean> get() = bool("amoledEnabled")
    val colorScheme: Flow<String> get() = string("colorScheme")
    val dynamicColor: Flow<Boolean> get() = bool("dynamicColor")
    val blurEffectsEnabled: Flow<Boolean> get() = bool("blurEffectsEnabled")
    val reduceMotion: Flow<Boolean> get() = bool("reduceMotion")
    val stickToBottom: Flow<Boolean> get() = bool("stickToBottom")
    val parseInlineDollarMath: Flow<Boolean> get() = bool("parseInlineDollarMath")
    val autoWrapCodeBlocks: Flow<Boolean> get() = bool("autoWrapCodeBlocks")
    val hapticsEnabled: Flow<Boolean> get() = bool("hapticsEnabled")
    val detailedTokenUsage: Flow<Boolean> get() = bool("detailedTokenUsage")
    val toolCallDisplayMode: Flow<String> get() = string("toolCallDisplayMode")
    val thinkingSegmentDisplayMode: Flow<String> get() = string("thinkingSegmentDisplayMode")
    val autoExpandActiveGroup: Flow<Boolean> get() = bool("autoExpandActiveGroup")
    val schemeStyle: Flow<String> get() = string("schemeStyle")
    val fontPreference: Flow<String> get() = string("fontPreference")
    val customFontName: Flow<String> get() = string("customFontName")
    val customFontPath: Flow<String> get() = string("customFontPath")
    val defaultTemperature: Flow<Float?> get() = nullableFloat("defaultTemperature")
    val defaultMaxTokens: Flow<Int?> get() = nullableInt("defaultMaxTokens")
    val defaultTopP: Flow<Float?> get() = nullableFloat("defaultTopP")
    val defaultFrequencyPenalty: Flow<Float?> get() = nullableFloat("defaultFrequencyPenalty")
    val defaultPresencePenalty: Flow<Float?> get() = nullableFloat("defaultPresencePenalty")
    val activeSystemPromptId: Flow<String?> get() = nullableString("activeSystemPromptId")

    // ------------------------------------------------------------ value shapes

    private fun record(name: String, value: JsonObject) {
        calls += buildJsonObject {
            put("name", name)
            put("value", value)
        }
    }

    private fun boolValue(value: Boolean): JsonObject = buildJsonObject { put("bool", value) }
    private fun intValue(value: Int): JsonObject = buildJsonObject { put("int", value) }
    private fun floatValue(value: Float?): JsonObject = buildJsonObject {
        if (value == null) put("float", JsonNull) else put("float", value.toString())
    }
    private fun stringValue(value: String?): JsonObject = buildJsonObject {
        if (value == null) put("string", JsonNull) else put("string", value)
    }
    private fun stringsValue(values: Collection<String>): JsonObject = buildJsonObject {
        put("strings", buildJsonArray { values.forEach { add(it) } })
    }
    private fun mapValue(values: Map<String, String>): JsonObject = buildJsonObject {
        put("map", buildJsonObject { values.forEach { (key, value) -> put(key, value) } })
    }

    private fun EmbeddingModelConfig.toJson(): JsonObject = buildJsonObject {
        put("id", id)
        put("name", name)
        put("type", type.name)
        put("remoteModelName", remoteModelName)
        put("remoteBaseUrl", remoteBaseUrl)
        put("remoteApiKey", remoteApiKey)
        put("localFilePath", localFilePath)
        put("batchSize", batchSize)
    }

    private fun ShellDeviceConfig.toJson(): JsonObject = buildJsonObject {
        put("id", id)
        put("name", name)
        put("description", description)
        put("type", type)
        put("serverUrl", serverUrl)
        put("apiKey", apiKey)
        put("conchPublicKey", conchPublicKey)
        put("sshHost", sshHost)
        put("sshPort", sshPort)
        put("sshUser", sshUser)
        put("sshPassword", sshPassword)
        put("sshHostKey", sshHostKey)
    }

    private fun McpServerConfig.toJson(): JsonObject = buildJsonObject {
        put("id", id)
        put("name", name)
        put("enabled", enabled)
        put("url", url)
        put("transport", transport.name)
        put("headers", buildJsonObject { headers.forEach { (key, value) -> put(key, value) } })
        put("disabledTools", buildJsonArray { disabledTools.forEach { add(it) } })
    }

    private fun CustomProviderConfig.toJson(): JsonObject = buildJsonObject {
        put("name", name)
        put("protocol", protocol.wireValue)
        put("id", id)
        put("legacyNames", buildJsonArray { legacyNames.forEach { add(it) } })
        put("responsesApiEnabled", responsesApiEnabled)
        put("anthropicCacheEnabled", anthropicCacheEnabled)
        put("anthropicCacheTtl", anthropicCacheTtl)
    }

    // ------------------------------------------------------------------ saves

    suspend fun saveSelectedModel(model: String) = record("saveSelectedModel", stringValue(model))
    suspend fun saveCustomModels(models: Set<String>) = record("saveCustomModels", stringsValue(models))
    suspend fun saveEnabledModels(models: Set<String>) = record("saveEnabledModels", stringsValue(models))
    suspend fun saveModelAliases(aliases: Map<String, String>) =
        record("saveModelAliases", mapValue(aliases))
    suspend fun saveModelProviderNames(values: Map<String, Boolean>, replace: Boolean = true) =
        record("saveModelProviderNames", buildJsonObject {
            put("map", buildJsonObject { values.forEach { (key, value) -> put(key, value) } })
            put("bool", replace)
        })
    suspend fun saveMaxContextWindow(window: Int) = record("saveMaxContextWindow", intValue(window))
    suspend fun saveVisualizeContextRollout(enabled: Boolean) =
        record("saveVisualizeContextRollout", boolValue(enabled))
    suspend fun saveContextCompactEnabled(enabled: Boolean) =
        record("saveContextCompactEnabled", boolValue(enabled))
    suspend fun saveContextCompactModel(model: String?) =
        record("saveContextCompactModel", stringValue(model))
    suspend fun saveContextCompactPrompt(prompt: String) =
        record("saveContextCompactPrompt", stringValue(prompt))
    suspend fun saveContextCompactRetainCount(count: Int) =
        record("saveContextCompactRetainCount", intValue(count))
    suspend fun saveContextCompactPreserveSystemPrompt(enabled: Boolean) =
        record("saveContextCompactPreserveSystemPrompt", boolValue(enabled))
    suspend fun saveContextCompactThresholdPercent(percent: Int) =
        record("saveContextCompactThresholdPercent", intValue(percent))
    suspend fun saveCodeExecutionEnabled(enabled: Boolean) =
        record("saveCodeExecutionEnabled", boolValue(enabled))
    suspend fun saveGoogleSearchEnabled(enabled: Boolean) =
        record("saveGoogleSearchEnabled", boolValue(enabled))
    suspend fun saveThinkingEnabled(enabled: Boolean) = record("saveThinkingEnabled", boolValue(enabled))
    suspend fun saveThinkingLevel(level: String) = record("saveThinkingLevel", stringValue(level))
    suspend fun saveThinkingBudgetEnabled(enabled: Boolean) =
        record("saveThinkingBudgetEnabled", boolValue(enabled))
    suspend fun saveThinkingBudgetTokens(tokens: Int) =
        record("saveThinkingBudgetTokens", intValue(tokens))
    suspend fun saveOpenAiServiceTierEnabled(enabled: Boolean) =
        record("saveOpenAiServiceTierEnabled", boolValue(enabled))
    suspend fun saveOpenAiServiceTier(tier: String) = record("saveOpenAiServiceTier", stringValue(tier))
    suspend fun saveOpenAiResponsesApiEnabled(enabled: Boolean) =
        record("saveOpenAiResponsesApiEnabled", boolValue(enabled))
    suspend fun saveAnthropicCacheEnabled(enabled: Boolean) =
        record("saveAnthropicCacheEnabled", boolValue(enabled))
    suspend fun saveAnthropicCacheTtl(ttl: String) = record("saveAnthropicCacheTtl", stringValue(ttl))
    suspend fun saveProviderBaseUrls(urls: Map<String, String>) =
        record("saveProviderBaseUrls", mapValue(urls))
    suspend fun saveTitleGenerationEnabled(enabled: Boolean) =
        record("saveTitleGenerationEnabled", boolValue(enabled))
    suspend fun saveTitleGenerationModel(model: String?) =
        record("saveTitleGenerationModel", stringValue(model))
    suspend fun saveTitleGenerationPrompt(prompt: String) =
        record("saveTitleGenerationPrompt", stringValue(prompt))
    suspend fun saveTitleGenerationNotificationsEnabled(enabled: Boolean) =
        record("saveTitleGenerationNotificationsEnabled", boolValue(enabled))
    suspend fun saveAccessPastConversations(enabled: Boolean) =
        record("saveAccessPastConversations", boolValue(enabled))
    suspend fun saveAccessSavedMemories(enabled: Boolean) =
        record("saveAccessSavedMemories", boolValue(enabled))
    suspend fun saveAccessActiveMemory(enabled: Boolean) =
        record("saveAccessActiveMemory", boolValue(enabled))
    suspend fun saveAccessSkills(enabled: Boolean) = record("saveAccessSkills", boolValue(enabled))
    suspend fun saveRagSearchEnabled(enabled: Boolean) =
        record("saveRagSearchEnabled", boolValue(enabled))
    suspend fun saveModelSearchMethod(method: String) =
        record("saveModelSearchMethod", stringValue(method))
    suspend fun saveManualSearchMethod(method: String) =
        record("saveManualSearchMethod", stringValue(method))
    suspend fun saveEmbeddingModels(models: List<EmbeddingModelConfig>) =
        record("saveEmbeddingModels", buildJsonObject {
            put("embeddingModels", buildJsonArray { models.forEach { add(it.toJson()) } })
        })
    suspend fun setActiveEmbeddingModelId(id: String) =
        record("setActiveEmbeddingModelId", stringValue(id))
    suspend fun saveAppLanguage(language: String) = record("saveAppLanguage", stringValue(language))
    suspend fun saveWebSearchEnabled(enabled: Boolean) =
        record("saveWebSearchEnabled", boolValue(enabled))
    suspend fun saveWebSearchProvider(provider: String) =
        record("saveWebSearchProvider", stringValue(provider))
    suspend fun saveWebSearchNumResults(n: Int) = record("saveWebSearchNumResults", intValue(n))
    suspend fun saveWebSearchBaseUrl(url: String) = record("saveWebSearchBaseUrl", stringValue(url))
    suspend fun saveImageGenEnabled(enabled: Boolean) =
        record("saveImageGenEnabled", boolValue(enabled))
    suspend fun saveImageGenModel(model: String?) = record("saveImageGenModel", stringValue(model))
    suspend fun saveImageGenSize(size: String) = record("saveImageGenSize", stringValue(size))
    suspend fun saveSearchContextWindow(n: Int) = record("saveSearchContextWindow", intValue(n))
    suspend fun saveSearchMatchLimit(n: Int) = record("saveSearchMatchLimit", intValue(n))
    suspend fun saveRagThreshold(threshold: Float) = record("saveRagThreshold", floatValue(threshold))
    suspend fun saveAutoCacheEnabled(enabled: Boolean) =
        record("saveAutoCacheEnabled", boolValue(enabled))
    suspend fun saveShowUncachedNotification(enabled: Boolean) =
        record("saveShowUncachedNotification", boolValue(enabled))
    suspend fun saveAutoUpdateCheck(enabled: Boolean) =
        record("saveAutoUpdateCheck", boolValue(enabled))
    suspend fun saveImageTranscriptionEnabled(enabled: Boolean) =
        record("saveImageTranscriptionEnabled", boolValue(enabled))
    suspend fun saveImageTranscriptionEnabledModels(models: Set<String>) =
        record("saveImageTranscriptionEnabledModels", stringsValue(models))
    suspend fun saveImageTranscriptionModel(model: String?) =
        record("saveImageTranscriptionModel", stringValue(model))
    suspend fun saveImageTranscriptionBatchSize(size: Int) =
        record("saveImageTranscriptionBatchSize", intValue(size))
    suspend fun saveImageTranscriptionPrompt(prompt: String) =
        record("saveImageTranscriptionPrompt", stringValue(prompt))
    suspend fun saveShellEnabled(enabled: Boolean) = record("saveShellEnabled", boolValue(enabled))
    suspend fun saveShellConfirmEnabled(enabled: Boolean) =
        record("saveShellConfirmEnabled", boolValue(enabled))
    suspend fun saveAskUserEnabled(enabled: Boolean) = record("saveAskUserEnabled", boolValue(enabled))
    suspend fun saveShellDevices(devices: List<ShellDeviceConfig>) =
        record("saveShellDevices", buildJsonObject {
            put("shellDevices", buildJsonArray { devices.forEach { add(it.toJson()) } })
        })
    suspend fun saveAutomationToolsEnabled(enabled: Boolean) =
        record("saveAutomationToolsEnabled", boolValue(enabled))
    suspend fun saveExactExecutionEnabled(enabled: Boolean) =
        record("saveExactExecutionEnabled", boolValue(enabled))
    suspend fun saveAutomationWakeLockEnabled(enabled: Boolean) =
        record("saveAutomationWakeLockEnabled", boolValue(enabled))
    suspend fun saveCustomProviders(providers: List<CustomProviderConfig>) =
        record("saveCustomProviders", buildJsonObject {
            put("customProviders", buildJsonArray { providers.forEach { add(it.toJson()) } })
        })
    suspend fun saveMcpServers(servers: List<McpServerConfig>) =
        record("saveMcpServers", buildJsonObject {
            put("mcpServers", buildJsonArray { servers.forEach { add(it.toJson()) } })
        })
    suspend fun saveProxyEnabled(enabled: Boolean) = record("saveProxyEnabled", boolValue(enabled))
    suspend fun saveProxyType(type: String) = record("saveProxyType", stringValue(type))
    suspend fun saveProxyHost(host: String) = record("saveProxyHost", stringValue(host))
    suspend fun saveProxyPort(port: String) = record("saveProxyPort", stringValue(port))
    suspend fun saveProxyUsername(user: String) = record("saveProxyUsername", stringValue(user))
    suspend fun saveProxyBypass(bypass: String) = record("saveProxyBypass", stringValue(bypass))
    suspend fun saveShowDocumentationFab(enabled: Boolean) =
        record("saveShowDocumentationFab", boolValue(enabled))
    suspend fun saveThemeMode(mode: String) = record("saveThemeMode", stringValue(mode))
    suspend fun saveAmoledEnabled(enabled: Boolean) = record("saveAmoledEnabled", boolValue(enabled))
    suspend fun saveColorScheme(scheme: String) = record("saveColorScheme", stringValue(scheme))
    suspend fun saveDynamicColor(enabled: Boolean) = record("saveDynamicColor", boolValue(enabled))
    suspend fun saveBlurEffectsEnabled(enabled: Boolean) =
        record("saveBlurEffectsEnabled", boolValue(enabled))
    suspend fun saveReduceMotion(enabled: Boolean) = record("saveReduceMotion", boolValue(enabled))
    suspend fun saveStickToBottom(enabled: Boolean) = record("saveStickToBottom", boolValue(enabled))
    suspend fun saveParseInlineDollarMath(enabled: Boolean) =
        record("saveParseInlineDollarMath", boolValue(enabled))
    suspend fun saveAutoWrapCodeBlocks(enabled: Boolean) =
        record("saveAutoWrapCodeBlocks", boolValue(enabled))
    suspend fun saveHapticsEnabled(enabled: Boolean) = record("saveHapticsEnabled", boolValue(enabled))
    suspend fun saveDetailedTokenUsage(enabled: Boolean) =
        record("saveDetailedTokenUsage", boolValue(enabled))
    suspend fun saveToolCallDisplayMode(mode: String) =
        record("saveToolCallDisplayMode", stringValue(mode))
    suspend fun saveThinkingSegmentDisplayMode(mode: String) =
        record("saveThinkingSegmentDisplayMode", stringValue(mode))
    suspend fun saveAutoExpandActiveGroup(enabled: Boolean) =
        record("saveAutoExpandActiveGroup", boolValue(enabled))
    suspend fun saveSchemeStyle(style: String) = record("saveSchemeStyle", stringValue(style))
    suspend fun saveFontPreference(value: String) = record("saveFontPreference", stringValue(value))
    suspend fun saveCustomFontPath(value: String) = record("saveCustomFontPath", stringValue(value))
    suspend fun saveCustomFontName(value: String) = record("saveCustomFontName", stringValue(value))
    suspend fun saveDefaultTemperature(value: Float?) =
        record("saveDefaultTemperature", floatValue(value))
    suspend fun saveDefaultMaxTokens(value: Int?) =
        record("saveDefaultMaxTokens", if (value == null) buildJsonObject { put("int", JsonNull) } else intValue(value))
    suspend fun saveDefaultTopP(value: Float?) = record("saveDefaultTopP", floatValue(value))
    suspend fun saveDefaultFrequencyPenalty(value: Float?) =
        record("saveDefaultFrequencyPenalty", floatValue(value))
    suspend fun saveDefaultPresencePenalty(value: Float?) =
        record("saveDefaultPresencePenalty", floatValue(value))
    suspend fun setActiveSystemPromptId(id: String?) =
        record("setActiveSystemPromptId", stringValue(id))
    suspend fun resetPortableSettingsForImport() =
        record("resetPortableSettingsForImport", buildJsonObject { put("none", true) })
    suspend fun invalidatePortableModelCaches() =
        record("invalidatePortableModelCaches", buildJsonObject { put("none", true) })
}
