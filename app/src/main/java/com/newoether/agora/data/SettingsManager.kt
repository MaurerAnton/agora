package com.newoether.agora.data

import android.content.Context
import com.newoether.agora.model.ThinkingSegmentDisplayModes
import com.newoether.agora.model.ToolCallDisplayModes
import com.newoether.agora.util.DebugLog
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.util.Locale
import kotlinx.coroutines.flow.map

internal val Context.dataStore by preferencesDataStore(
    name = "settings", produceMigrations = { listOf(modelProviderNamesMigration, toolCallDisplayModeMigration) },
)

class SettingsManager(private val context: Context) {
    private val json = Json { ignoreUnknownKeys = true }
    internal val modelPreferenceStore = SettingsModelPreferenceStore(context.dataStore, json)
    internal val backupPreferenceStore = SettingsBackupPreferenceStore(context.dataStore)
    internal val systemPromptStore = SettingsSystemPromptStore(context.dataStore, json)
    internal val shellDeviceStore = SettingsShellDeviceStore(context.dataStore, json)
    /** Generation, retrieval and per-request parameters live in SettingsGenerationParameters.kt. */
    private val generationParameters = SettingsGenerationParametersStore(context.dataStore, json)

    companion object {
        const val DEFAULT_PROXY_HOST = "127.0.0.1"
        const val DEFAULT_PROXY_PORT = "7890"
        const val DEFAULT_PROXY_BYPASS =
            "localhost\n127.0.0.1\n10.0.0.0/8\n172.16.0.0/12\n192.168.0.0/16\n::1"
        // Tor toggle (June feature): kept so the Tor settings page keeps working.
        val TOR_ENABLED = booleanPreferencesKey("tor_enabled")
        val TOR_SOCKS_PORT = intPreferencesKey("tor_socks_port")
    }

    val selectedModel: Flow<String> = modelPreferenceStore.selectedModel
    val anthropicCacheEnabled: Flow<Boolean> = modelPreferenceStore.anthropicCacheEnabled
    val anthropicCacheTtl: Flow<String> = modelPreferenceStore.anthropicCacheTtl
    suspend fun saveAnthropicCacheEnabled(enabled: Boolean) = modelPreferenceStore.saveAnthropicCacheEnabled(enabled)
    suspend fun saveAnthropicCacheTtl(ttl: String) = modelPreferenceStore.saveAnthropicCacheTtl(ttl)
    suspend fun updateCustomProviderCache(providerId: String, enabled: Boolean? = null, ttl: String? = null) =
        modelPreferenceStore.updateCustomProviderCache(providerId, enabled, ttl)
    val providerBaseUrls: Flow<Map<String, String>> = modelPreferenceStore.providerBaseUrls
    val customEndpointResolutions: Flow<Map<String, CustomEndpointResolution>> =
        modelPreferenceStore.customEndpointResolutions
    val availableModels: Flow<Map<String, List<String>>> = modelPreferenceStore.availableModels
    val customModels: Flow<Set<String>> = modelPreferenceStore.customModels
    val enabledModels: Flow<Set<String>> = modelPreferenceStore.enabledModels
    val modelAliases: Flow<Map<String, String>> = modelPreferenceStore.modelAliases
    val modelProviderNames: Flow<Map<String, Boolean>> = modelPreferenceStore.modelProviderNames
    val apiKeys: Flow<List<ApiKeyEntry>> = modelPreferenceStore.apiKeys
    val activeApiKeyIds: Flow<Map<String, String>> = modelPreferenceStore.activeApiKeyIds

    val systemPrompts: Flow<List<SystemPromptEntry>> = systemPromptStore.systemPrompts
    val activeSystemPromptId: Flow<String?> = systemPromptStore.activeSystemPromptId

    val maxContextWindow: Flow<Int> = generationParameters.maxContextWindow
    val visualizeContextRollout: Flow<Boolean> = generationParameters.visualizeContextRollout
    val contextCompactEnabled: Flow<Boolean> = generationParameters.contextCompactEnabled
    val contextCompactModel: Flow<String?> = generationParameters.contextCompactModel
    val contextCompactPrompt: Flow<String> = generationParameters.contextCompactPrompt
    val contextCompactRetainCount: Flow<Int> = generationParameters.contextCompactRetainCount
    val contextCompactPreserveSystemPrompt: Flow<Boolean> =
        generationParameters.contextCompactPreserveSystemPrompt
    val contextCompactThresholdPercent: Flow<Int> = generationParameters.contextCompactThresholdPercent
    val codeExecutionEnabled: Flow<Boolean> = generationParameters.codeExecutionEnabled
    val googleSearchEnabled: Flow<Boolean> = generationParameters.googleSearchEnabled
    val thinkingEnabled: Flow<Boolean> = generationParameters.thinkingEnabled
    val autoRegenOnCjk: Flow<Boolean> = generationParameters.autoRegenOnCjk
    val maxCjkRetries: Flow<Int> = generationParameters.maxCjkRetries
    val thinkingLevel: Flow<String> = generationParameters.thinkingLevel
    val thinkingBudgetEnabled: Flow<Boolean> = generationParameters.thinkingBudgetEnabled
    val thinkingBudgetTokens: Flow<Int> = generationParameters.thinkingBudgetTokens
    val openAiServiceTierEnabled: Flow<Boolean> = generationParameters.openAiServiceTierEnabled
    val openAiServiceTier: Flow<String> = generationParameters.openAiServiceTier
    val openAiResponsesApiEnabled: Flow<Boolean> = generationParameters.openAiResponsesApiEnabled
    val titleGenerationEnabled: Flow<Boolean> = generationParameters.titleGenerationEnabled
    val titleGenerationModel: Flow<String?> = generationParameters.titleGenerationModel
    val titleGenerationPrompt: Flow<String> = generationParameters.titleGenerationPrompt
    val titleGenerationNotificationsEnabled: Flow<Boolean> =
        generationParameters.titleGenerationNotificationsEnabled
    val imageTranscriptionEnabled: Flow<Boolean> = generationParameters.imageTranscriptionEnabled
    val imageTranscriptionEnabledModels: Flow<Set<String>> = generationParameters.imageTranscriptionEnabledModels
    val imageTranscriptionModel: Flow<String?> = generationParameters.imageTranscriptionModel
    val imageTranscriptionBatchSize: Flow<Int> = generationParameters.imageTranscriptionBatchSize
    val imageTranscriptionPrompt: Flow<String> = generationParameters.imageTranscriptionPrompt

    val accessPastConversations: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_PAST_CONVERSATIONS] ?: true }
    val accessSavedMemories: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_SAVED_MEMORIES] ?: true }
    val accessActiveMemory: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_ACTIVE_MEMORY] ?: true }
    val accessSkills: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_SKILLS] ?: true }
    /** Modify access follows the legacy single access_skills value until explicitly set. */
    val accessSkillsModify: Flow<Boolean> = context.dataStore.data.map { pref ->
        pref[ACCESS_SKILLS_MODIFY] ?: pref[ACCESS_SKILLS] ?: true
    }
    val ragSearchEnabled: Flow<Boolean> = generationParameters.ragSearchEnabled
    val modelSearchMethod: Flow<String> = generationParameters.modelSearchMethod
    val manualSearchMethod: Flow<String> = generationParameters.manualSearchMethod
    val embeddingModels: Flow<List<EmbeddingModelConfig>> = generationParameters.embeddingModels
    val activeEmbeddingModelId: Flow<String> = generationParameters.activeEmbeddingModelId

    val appLanguage: Flow<String> = context.dataStore.data.map { it[APP_LANGUAGE] ?: "system" }
    val webSearchEnabled: Flow<Boolean> = generationParameters.webSearchEnabled
    val webSearchProvider: Flow<String> = generationParameters.webSearchProvider
    val webSearchApiKeys: Flow<Map<String, String>> = generationParameters.webSearchApiKeys
    val webSearchNumResults: Flow<Int> = generationParameters.webSearchNumResults
    val webSearchBaseUrl: Flow<String> = generationParameters.webSearchBaseUrl

    // ── Image generation ──────────────────────────────────────
    val imageQuality = ImageQualitySettingsStore(context.dataStore)
    val imageGenEnabled: Flow<Boolean> = generationParameters.imageGenEnabled
    val imageGenModel: Flow<String?> = generationParameters.imageGenModel
    val imageGenSize: Flow<String> = generationParameters.imageGenSize
    val searchContextWindow: Flow<Int> = generationParameters.searchContextWindow
    val searchMatchLimit: Flow<Int> = generationParameters.searchMatchLimit
    val ragThreshold: Flow<Float> = generationParameters.ragThreshold
    val defaultTemperature: Flow<Float?> = generationParameters.defaultTemperature
    val defaultMaxTokens: Flow<Int?> = generationParameters.defaultMaxTokens
    val defaultTopP: Flow<Float?> = generationParameters.defaultTopP
    val defaultFrequencyPenalty: Flow<Float?> = generationParameters.defaultFrequencyPenalty
    val defaultPresencePenalty: Flow<Float?> = generationParameters.defaultPresencePenalty
    val conversationSettings: Flow<Map<String, ConversationSettings>> = generationParameters.conversationSettings
    val autoCacheEnabled: Flow<Boolean> = context.dataStore.data.map { it[AUTO_CACHE_ENABLED] ?: true }
    val showUncachedNotification: Flow<Boolean> =
        context.dataStore.data.map { it[SHOW_UNCACHED_NOTIFICATION] ?: true }
    val autoUpdateCheck: Flow<Boolean> = context.dataStore.data.map { it[AUTO_UPDATE_CHECK] ?: true }
    val lastUpdateCheckTime: Flow<Long> = context.dataStore.data.map { it[LAST_UPDATE_CHECK_TIME] ?: 0L }
    val localChatModels: Flow<List<LocalChatModelConfig>> = modelPreferenceStore.localChatModels
    val phoneUse = com.newoether.agora.phone.PhoneUseSettingsStore(context.dataStore)
    val localModelIdleRetentionMinutes: Flow<Int> = modelPreferenceStore.localModelIdleRetentionMinutes
    val localLowContextModeEnabled: Flow<Boolean> = modelPreferenceStore.localLowContextModeEnabled
    val customProviders: Flow<List<CustomProviderConfig>> = modelPreferenceStore.customProviders

    val showDocumentationFab: Flow<Boolean> = context.dataStore.data.map { it[SHOW_DOCUMENTATION_FAB] ?: true }
    /** Release-build feature gate kept local to this installation. */
    val developerOptionsEnabled: Flow<Boolean> =
        context.dataStore.data.map { it[DEVELOPER_OPTIONS_ENABLED] ?: false }
    val debugModelEnabled: Flow<Boolean> = context.dataStore.data.map { preferences ->
        (preferences[DEVELOPER_OPTIONS_ENABLED] ?: false) &&
            (preferences[DEBUG_MODEL_ENABLED] ?: false)
    }

    val shellEnabled: Flow<Boolean> = context.dataStore.data.map { it[SHELL_ENABLED] ?: true }
    val automationToolsEnabled: Flow<Boolean> = context.dataStore.data.map { it[AUTOMATION_TOOLS_ENABLED] ?: false }
    val exactExecutionEnabled: Flow<Boolean> = context.dataStore.data.map { it[EXACT_EXECUTION_ENABLED] ?: false }
    val automationWakeLockEnabled: Flow<Boolean> =
        context.dataStore.data.map { it[AUTOMATION_WAKE_LOCK_ENABLED] ?: false }
    val proxyEnabled: Flow<Boolean> = context.dataStore.data.map { it[PROXY_ENABLED] ?: false }
    val proxyType: Flow<String> = context.dataStore.data.map { it[PROXY_TYPE] ?: "http" }
    val proxyHost: Flow<String> = context.dataStore.data.map { it[PROXY_HOST] ?: DEFAULT_PROXY_HOST }
    val proxyPort: Flow<String> = context.dataStore.data.map { it[PROXY_PORT] ?: DEFAULT_PROXY_PORT }
    val proxyUsername: Flow<String> = context.dataStore.data.map { it[PROXY_USERNAME] ?: "" }
    val proxyPassword: Flow<String> = context.dataStore.data.map { it[PROXY_PASSWORD] ?: "" }
    val proxyBypass: Flow<String> = context.dataStore.data.map { it[PROXY_BYPASS] ?: DEFAULT_PROXY_BYPASS }
    // Confirm before the model runs state-changing commands on remote shell servers. Default on.
    val shellConfirmEnabled: Flow<Boolean> = context.dataStore.data.map { it[SHELL_CONFIRM_ENABLED] ?: true }
    val askUserEnabled: Flow<Boolean> = context.dataStore.data.map { it[ASK_USER_ENABLED] ?: true }
    val askUserTimeoutMinutes: Flow<Int> = context.dataStore.data.map { normalizeAskUserTimeoutMinutes(it[ASK_USER_TIMEOUT_MINUTES]) }
    val todoEnabled: Flow<Boolean> = context.dataStore.data.map { it[TODO_ENABLED] ?: true }
    val subagentToolsEnabled: Flow<Boolean> = context.dataStore.data.map { it[SUBAGENT_TOOLS_ENABLED] ?: true }
    val subagentDeleteEnabled: Flow<Boolean> = context.dataStore.data.map { it[SUBAGENT_DELETE_ENABLED] ?: false }
    val subagentMaxTotal: Flow<Int> = context.dataStore.data.map { normalizeSubagentMaxTotal(it[SUBAGENT_MAX_TOTAL]) }
    val subagentMaxConcurrent: Flow<Int> = context.dataStore.data.map {
        normalizeSubagentMaxConcurrent(it[SUBAGENT_MAX_CONCURRENT], normalizeSubagentMaxTotal(it[SUBAGENT_MAX_TOTAL]))
    }
    val subagentCapsuleDisplayMode: Flow<String> = context.dataStore.data.map {
        SubagentCapsuleDisplayModes.normalize(it[SUBAGENT_CAPSULE_DISPLAY_MODE])
    }
    val subagentEnabledModels: Flow<Set<String>?> = modelPreferenceStore.subagentEnabledModels
    val subagentRecommendedModel: Flow<String?> = modelPreferenceStore.subagentRecommendedModel
    val subagentModelIds: Flow<Set<String>> = modelPreferenceStore.subagentModelIds
    suspend fun addSubagentModels(models: Set<String>) = modelPreferenceStore.addSubagentModels(models)
    suspend fun removeSubagentModel(model: String) = modelPreferenceStore.removeSubagentModel(model)
    suspend fun saveSubagentEnabledModels(models: Set<String>?, merge: Boolean = false) =
        modelPreferenceStore.saveSubagentEnabledModels(models, merge)
    suspend fun saveSubagentRecommendedModel(model: String?) = modelPreferenceStore.saveSubagentRecommendedModel(model)
    val shellDevices: Flow<List<ShellDeviceConfig>> =
        context.dataStore.data.map { preferences -> decodeEncryptedShellDevices(preferences, json) }
    val mcpServers: Flow<List<McpServerConfig>> = context.dataStore.data.map { pref ->
        val jsonStr = com.newoether.agora.util.SecretCrypto.decrypt(pref[MCP_SERVERS_JSON] ?: "[]")
        try {
            json.decodeFromString<List<McpServerConfig>>(jsonStr)
        } catch (e: Exception) {
            DebugLog.e("SettingsManager", "Failed to decode MCP servers", e)
            emptyList()
        }
    }
    val sandboxEnabled: Flow<Boolean> = context.dataStore.data.map { it[SANDBOX_ENABLED] ?: false }
    val sandboxSharedStorageEnabled: Flow<Boolean> =
        context.dataStore.data.map { it[SANDBOX_SHARED_STORAGE_ENABLED] ?: false }

    val themeMode: Flow<String> = context.dataStore.data.map { it[THEME_MODE] ?: "FOLLOW_DEVICE" }
    val amoledEnabled: Flow<Boolean> = context.dataStore.data.map { it[AMOLED_ENABLED] ?: false }
    val colorScheme: Flow<String> = context.dataStore.data.map {
        it[COLOR_SCHEME] ?: DEFAULT_COLOR_SCHEME
    }
    val dynamicColor: Flow<Boolean> = context.dataStore.data.map {
        it[DYNAMIC_COLOR] ?: DEFAULT_DYNAMIC_COLOR
    }
    val blurEffectsEnabled: Flow<Boolean> = context.dataStore.data.map { it[BLUR_EFFECTS_ENABLED] ?: true }
    val reduceMotion: Flow<Boolean> = context.dataStore.data.map { it[REDUCE_MOTION] ?: false }
    val stickToBottom: Flow<Boolean> = context.dataStore.data.map { it[STICK_TO_BOTTOM] ?: true }
    val parseInlineDollarMath: Flow<Boolean> = context.dataStore.data.map { it[PARSE_INLINE_DOLLAR_MATH] ?: false }
    val autoWrapCodeBlocks: Flow<Boolean> = context.dataStore.data.map { it[AUTO_WRAP_CODE_BLOCKS] ?: true }
    val hapticsEnabled: Flow<Boolean> = context.dataStore.data.map { it[HAPTICS_ENABLED] ?: true }
    val detailedTokenUsage: Flow<Boolean> = context.dataStore.data.map { it[DETAILED_TOKEN_USAGE] ?: false }
    val toolCallDisplayMode: Flow<String> = context.dataStore.data.map { ToolCallDisplayModes.normalize(it[TOOL_CALL_DISPLAY_MODE]) }
    val thinkingSegmentDisplayMode: Flow<String> = context.dataStore.data.map {
        ThinkingSegmentDisplayModes.normalize(it[THINKING_SEGMENT_DISPLAY_MODE])
    }
    val autoExpandActiveGroup: Flow<Boolean> = context.dataStore.data.map { it[AUTO_EXPAND_ACTIVE_GROUP] ?: true }
    val schemeStyle: Flow<String> = context.dataStore.data.map { it[SCHEME_STYLE] ?: DEFAULT_SCHEME_STYLE }
    val fontPreference: Flow<String> = context.dataStore.data.map { it[FONT_PREFERENCE] ?: "app_default" }
    val customFontPath: Flow<String> = context.dataStore.data.map { it[CUSTOM_FONT_PATH] ?: "" }
    val customFontName: Flow<String> = context.dataStore.data.map { it[CUSTOM_FONT_NAME] ?: "" }
    val firstLaunchTime: Flow<Long?> = context.dataStore.data.map { it[FIRST_LAUNCH_TIME] }
    val onboardingCompleted: Flow<Boolean> = context.dataStore.data.map { it[ONBOARDING_COMPLETED] ?: false }
    val ratingPromptSubmitted: Flow<Boolean> = context.dataStore.data.map { it[RATING_PROMPT_SUBMITTED] ?: false }
    val ratingPromptDismissed: Flow<Boolean> = context.dataStore.data.map { it[RATING_PROMPT_DISMISSED] ?: false }
    val totalMessagesSent: Flow<Int> = context.dataStore.data.map { it[TOTAL_MESSAGES_SENT] ?: 0 }

    val torEnabled: Flow<Boolean> = context.dataStore.data.map { it[TOR_ENABLED] ?: false }
    val torSocksPort: Flow<Int> = context.dataStore.data.map { it[TOR_SOCKS_PORT] ?: 9050 }

    // ── Auto Backup ───────────────────────────────────────────
    val autoBackupEnabled: Flow<Boolean> = backupPreferenceStore.autoBackupEnabled
    val autoBackupPeriodHours: Flow<Int> = backupPreferenceStore.autoBackupPeriodHours
    val autoBackupCategories: Flow<String> = backupPreferenceStore.autoBackupCategories
    val autoBackupDirectory: Flow<String> = backupPreferenceStore.autoBackupDirectory
    val autoDeleteEnabled: Flow<Boolean> = backupPreferenceStore.autoDeleteEnabled
    val autoDeletePeriodHours: Flow<Int> = backupPreferenceStore.autoDeletePeriodHours
    val lastBackupTimestamp: Flow<Long> = backupPreferenceStore.lastBackupTimestamp
    val lastModelsFetchFingerprint: Flow<String> = modelPreferenceStore.lastModelsFetchFingerprint

    suspend fun saveProviderBaseUrl(provider: String, url: String) =
        modelPreferenceStore.saveProviderBaseUrl(provider, url)

    suspend fun saveProviderBaseUrls(urls: Map<String, String>) =
        modelPreferenceStore.saveProviderBaseUrls(urls)

    suspend fun saveCustomEndpointResolution(
        provider: String,
        resolution: CustomEndpointResolution?,
    ) = modelPreferenceStore.saveCustomEndpointResolution(provider, resolution)

    suspend fun renameCustomEndpointResolution(oldName: String, newName: String) =
        modelPreferenceStore.renameCustomEndpointResolution(oldName, newName)

    suspend fun saveSelectedModel(model: String) =
        modelPreferenceStore.saveSelectedModel(model)

    suspend fun saveAvailableModels(provider: String, models: List<String>) =
        modelPreferenceStore.saveAvailableModels(provider, models)

    suspend fun saveCustomModels(models: Set<String>) =
        modelPreferenceStore.saveCustomModels(models)

    suspend fun addCustomModel(modelId: String, alias: String, showProviderName: Boolean = true) =
        modelPreferenceStore.addCustomModel(modelId, alias, showProviderName)

    suspend fun replaceCustomModel(
        oldModelId: String,
        newModelId: String?,
        alias: String,
        showProviderName: Boolean? = null,
    ) = modelPreferenceStore.replaceCustomModel(oldModelId, newModelId, alias, showProviderName)

    suspend fun saveEnabledModels(models: Set<String>) =
        modelPreferenceStore.saveEnabledModels(models)

    suspend fun saveModelAliases(aliases: Map<String, String>) =
        modelPreferenceStore.saveModelAliases(aliases)

    suspend fun updateModelAlias(modelId: String, alias: String, showProviderName: Boolean? = null) =
        modelPreferenceStore.updateModelAlias(modelId, alias, showProviderName)

    suspend fun saveModelProviderNames(values: Map<String, Boolean>, replace: Boolean = true) =
        modelPreferenceStore.saveModelProviderNames(values, replace)

    suspend fun synchronizeLocalModelAliases(aliases: Map<String, String>) =
        modelPreferenceStore.synchronizeLocalModelAliases(aliases)

    suspend fun removeModelAliasesForProvider(providerId: String) =
        modelPreferenceStore.removeModelAliasesForProvider(providerId)

    suspend fun saveApiKeys(keys: List<ApiKeyEntry>) =
        modelPreferenceStore.saveApiKeys(keys)

    suspend fun saveActiveApiKeyIds(ids: Map<String, String>) =
        modelPreferenceStore.saveActiveApiKeyIds(ids)

    suspend fun setActiveApiKeyId(provider: String, id: String?) =
        modelPreferenceStore.setActiveApiKeyId(provider, id)

    suspend fun renameApiKeyProvider(oldProvider: String, newProvider: String) =
        modelPreferenceStore.renameApiKeyProvider(oldProvider, newProvider)

    suspend fun saveSystemPrompts(prompts: List<SystemPromptEntry>) = systemPromptStore.save(prompts)
    suspend fun initializeFirstInstallDefaults(
        locale: Locale = Locale.getDefault(),
        now: Long = System.currentTimeMillis()
    ) = systemPromptStore.initialize(locale, now)

    suspend fun setActiveSystemPromptId(id: String?) = systemPromptStore.setActive(id)
    suspend fun saveMaxContextWindow(window: Int) = generationParameters.saveMaxContextWindow(window)
    suspend fun saveVisualizeContextRollout(enabled: Boolean) =
        generationParameters.saveVisualizeContextRollout(enabled)
    suspend fun saveCodeExecutionEnabled(enabled: Boolean) = generationParameters.saveCodeExecutionEnabled(enabled)
    suspend fun saveGoogleSearchEnabled(enabled: Boolean) = generationParameters.saveGoogleSearchEnabled(enabled)
    suspend fun saveThinkingEnabled(enabled: Boolean) = generationParameters.saveThinkingEnabled(enabled)
    suspend fun saveAutoRegenOnCjk(enabled: Boolean) = generationParameters.saveAutoRegenOnCjk(enabled)
    suspend fun saveMaxCjkRetries(retries: Int) = generationParameters.saveMaxCjkRetries(retries)
    suspend fun saveThinkingLevel(level: String) = generationParameters.saveThinkingLevel(level)
    suspend fun saveThinkingBudgetEnabled(enabled: Boolean) = generationParameters.saveThinkingBudgetEnabled(enabled)
    suspend fun saveThinkingBudgetTokens(tokens: Int) = generationParameters.saveThinkingBudgetTokens(tokens)
    suspend fun saveOpenAiServiceTierEnabled(enabled: Boolean) =
        generationParameters.saveOpenAiServiceTierEnabled(enabled)
    suspend fun saveOpenAiServiceTier(tier: String) = generationParameters.saveOpenAiServiceTier(tier)
    suspend fun saveOpenAiResponsesApiEnabled(enabled: Boolean) =
        generationParameters.saveOpenAiResponsesApiEnabled(enabled)
    suspend fun saveTitleGenerationEnabled(enabled: Boolean) = generationParameters.saveTitleGenerationEnabled(enabled)
    suspend fun saveTitleGenerationNotificationsEnabled(enabled: Boolean) =
        generationParameters.saveTitleGenerationNotificationsEnabled(enabled)
    suspend fun saveAccessPastConversations(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_PAST_CONVERSATIONS] = enabled }
    }
    suspend fun saveAccessSavedMemories(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_SAVED_MEMORIES] = enabled }
    }
    suspend fun saveAccessActiveMemory(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_ACTIVE_MEMORY] = enabled }
    }
    suspend fun saveAccessSkills(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_SKILLS] = enabled }
    }
    suspend fun saveAccessSkillsModify(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_SKILLS_MODIFY] = enabled }
    }
    suspend fun saveRagSearchEnabled(enabled: Boolean) = generationParameters.saveRagSearchEnabled(enabled)
    suspend fun saveAutoCacheEnabled(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_CACHE_ENABLED] = enabled }
    }
    suspend fun saveShowUncachedNotification(enabled: Boolean) {
        context.dataStore.edit { it[SHOW_UNCACHED_NOTIFICATION] = enabled }
    }
    suspend fun saveAutoUpdateCheck(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_UPDATE_CHECK] = enabled }
    }
    suspend fun saveLastUpdateCheckTime(time: Long) {
        context.dataStore.edit { it[LAST_UPDATE_CHECK_TIME] = time }
    }
    suspend fun saveModelSearchMethod(method: String) = generationParameters.saveModelSearchMethod(method)
    suspend fun saveManualSearchMethod(method: String) = generationParameters.saveManualSearchMethod(method)
    suspend fun saveEmbeddingModels(models: List<EmbeddingModelConfig>) =
        generationParameters.saveEmbeddingModels(models)
    suspend fun setActiveEmbeddingModelId(id: String) = generationParameters.setActiveEmbeddingModelId(id)
    suspend fun saveAppLanguage(language: String) {
        context.dataStore.edit { it[APP_LANGUAGE] = language }
    }
    suspend fun saveWebSearchEnabled(enabled: Boolean) = generationParameters.saveWebSearchEnabled(enabled)
    suspend fun saveWebSearchProvider(provider: String) = generationParameters.saveWebSearchProvider(provider)
    suspend fun saveWebSearchApiKey(provider: String, apiKey: String) =
        generationParameters.saveWebSearchApiKey(provider, apiKey)
    suspend fun saveWebSearchApiKeys(keys: Map<String, String>) = generationParameters.saveWebSearchApiKeys(keys)
    suspend fun saveWebSearchNumResults(n: Int) = generationParameters.saveWebSearchNumResults(n)
    suspend fun saveWebSearchBaseUrl(url: String) = generationParameters.saveWebSearchBaseUrl(url)
    suspend fun saveImageGenEnabled(enabled: Boolean) = generationParameters.saveImageGenEnabled(enabled)
    suspend fun saveImageGenModel(model: String?) = generationParameters.saveImageGenModel(model)
    suspend fun saveImageGenSize(size: String) = generationParameters.saveImageGenSize(size)
    suspend fun saveSearchMatchLimit(n: Int) = generationParameters.saveSearchMatchLimit(n)
    suspend fun saveSearchContextWindow(n: Int) = generationParameters.saveSearchContextWindow(n)
    suspend fun saveRagThreshold(threshold: Float) = generationParameters.saveRagThreshold(threshold)
    suspend fun saveDefaultTemperature(value: Float?) = generationParameters.saveDefaultTemperature(value)
    suspend fun saveDefaultMaxTokens(value: Int?) = generationParameters.saveDefaultMaxTokens(value)
    suspend fun saveDefaultTopP(value: Float?) = generationParameters.saveDefaultTopP(value)
    suspend fun saveDefaultFrequencyPenalty(value: Float?) = generationParameters.saveDefaultFrequencyPenalty(value)
    suspend fun saveDefaultPresencePenalty(value: Float?) = generationParameters.saveDefaultPresencePenalty(value)
    suspend fun saveConversationSettings(
        conversationId: String,
        settings: ConversationSettings?,
    ): Map<String, ConversationSettings> = generationParameters.saveConversationSettings(conversationId, settings)
    suspend fun saveConversationSettingsMap(settings: Map<String, ConversationSettings>) =
        generationParameters.saveConversationSettingsMap(settings)
    suspend fun saveLocalChatModels(models: List<LocalChatModelConfig>) =
        modelPreferenceStore.saveLocalChatModels(models)

    suspend fun saveLocalModelIdleRetentionMinutes(minutes: Int) =
        modelPreferenceStore.saveLocalModelIdleRetentionMinutes(minutes)

    suspend fun saveLocalLowContextModeEnabled(enabled: Boolean) =
        modelPreferenceStore.saveLocalLowContextModeEnabled(enabled)

    suspend fun saveCustomProviders(providers: List<CustomProviderConfig>) =
        modelPreferenceStore.saveCustomProviders(providers)

    internal suspend fun normalizeCustomProviderIdentities(): List<CustomProviderIdentityMigration> =
        modelPreferenceStore.normalizeCustomProviderIdentities()

    internal suspend fun clearLegacyCustomProviderNames(
        completed: List<CustomProviderIdentityMigration>,
    ) = modelPreferenceStore.clearLegacyCustomProviderNames(completed)

    suspend fun saveContextCompactEnabled(enabled: Boolean) = generationParameters.saveContextCompactEnabled(enabled)
    suspend fun saveContextCompactModel(model: String?) = generationParameters.saveContextCompactModel(model)
    suspend fun saveContextCompactPrompt(prompt: String) = generationParameters.saveContextCompactPrompt(prompt)
    suspend fun saveContextCompactRetainCount(count: Int) = generationParameters.saveContextCompactRetainCount(count)
    suspend fun saveContextCompactPreserveSystemPrompt(enabled: Boolean) =
        generationParameters.saveContextCompactPreserveSystemPrompt(enabled)

    suspend fun saveContextCompactThresholdPercent(percent: Int) =
        generationParameters.saveContextCompactThresholdPercent(percent)

    suspend fun saveTitleGenerationModel(model: String?) = generationParameters.saveTitleGenerationModel(model)
    suspend fun saveTitleGenerationPrompt(prompt: String) = generationParameters.saveTitleGenerationPrompt(prompt)
    suspend fun saveImageTranscriptionEnabledModels(models: Set<String>) =
        generationParameters.saveImageTranscriptionEnabledModels(models)
    suspend fun saveImageTranscriptionEnabled(enabled: Boolean) =
        generationParameters.saveImageTranscriptionEnabled(enabled)
    suspend fun saveImageTranscriptionModel(model: String?) = generationParameters.saveImageTranscriptionModel(model)
    suspend fun saveImageTranscriptionBatchSize(size: Int) = generationParameters.saveImageTranscriptionBatchSize(size)
    suspend fun saveImageTranscriptionPrompt(prompt: String) = generationParameters.saveImageTranscriptionPrompt(prompt)
    suspend fun saveShowDocumentationFab(enabled: Boolean) {
        context.dataStore.edit { it[SHOW_DOCUMENTATION_FAB] = enabled }
    }
    suspend fun saveDeveloperOptionsEnabled(enabled: Boolean) {
        context.dataStore.edit {
            it[DEVELOPER_OPTIONS_ENABLED] = enabled
            if (!enabled) {
                it[DEBUG_MODEL_ENABLED] = false
            }
        }
    }
    suspend fun saveDebugModelEnabled(enabled: Boolean) {
        context.dataStore.edit {
            it[DEBUG_MODEL_ENABLED] =
                enabled && (it[DEVELOPER_OPTIONS_ENABLED] ?: false)
        }
    }
    suspend fun saveShellEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SHELL_ENABLED] = enabled }
    }
    suspend fun saveAutomationToolsEnabled(enabled: Boolean) {
        context.dataStore.edit { it[AUTOMATION_TOOLS_ENABLED] = enabled }
    }
    suspend fun saveExactExecutionEnabled(enabled: Boolean) {
        context.dataStore.edit { it[EXACT_EXECUTION_ENABLED] = enabled }
    }
    suspend fun saveAutomationWakeLockEnabled(enabled: Boolean) {
        context.dataStore.edit { it[AUTOMATION_WAKE_LOCK_ENABLED] = enabled }
    }
    suspend fun saveProxyEnabled(enabled: Boolean) { context.dataStore.edit { it[PROXY_ENABLED] = enabled } }
    suspend fun saveProxyType(type: String) { context.dataStore.edit { it[PROXY_TYPE] = type } }
    suspend fun saveProxyHost(host: String) { context.dataStore.edit { it[PROXY_HOST] = host } }
    suspend fun saveProxyPort(port: String) { context.dataStore.edit { it[PROXY_PORT] = port } }
    suspend fun saveProxyUsername(user: String) { context.dataStore.edit { it[PROXY_USERNAME] = user } }
    suspend fun saveProxyPassword(pass: String) { context.dataStore.edit { it[PROXY_PASSWORD] = pass } }
    suspend fun saveProxyBypass(bypass: String) { context.dataStore.edit { it[PROXY_BYPASS] = bypass } }

    suspend fun saveShellConfirmEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SHELL_CONFIRM_ENABLED] = enabled }
    }
    suspend fun saveAskUserEnabled(enabled: Boolean) {
        context.dataStore.edit { it[ASK_USER_ENABLED] = enabled }
    }
    suspend fun saveAskUserTimeoutMinutes(minutes: Int) {
        context.dataStore.edit { it[ASK_USER_TIMEOUT_MINUTES] = normalizeAskUserTimeoutMinutes(minutes) }
    }
    suspend fun saveTodoEnabled(enabled: Boolean) {
        context.dataStore.edit { it[TODO_ENABLED] = enabled }
    }
    suspend fun saveSubagentMaxTotal(value: Int) = saveSubagentLimits(maxTotal = value)
    suspend fun saveSubagentMaxConcurrent(value: Int) = saveSubagentLimits(maxConcurrent = value)
    internal suspend fun saveSubagentLimits(maxTotal: Int? = null, maxConcurrent: Int? = null) {
        context.dataStore.edit { prefs ->
            val previousTotal = normalizeSubagentMaxTotal(prefs[SUBAGENT_MAX_TOTAL])
            val total = normalizeSubagentMaxTotal(maxTotal ?: previousTotal)
            val concurrent = maxConcurrent ?: normalizeSubagentMaxConcurrent(prefs[SUBAGENT_MAX_CONCURRENT], previousTotal)
            prefs[SUBAGENT_MAX_TOTAL] = total
            prefs[SUBAGENT_MAX_CONCURRENT] = normalizeSubagentMaxConcurrent(concurrent, total)
        }
    }
    suspend fun saveSubagentCapsuleDisplayMode(mode: String) {
        context.dataStore.edit { it[SUBAGENT_CAPSULE_DISPLAY_MODE] = SubagentCapsuleDisplayModes.normalize(mode) }
    }
    suspend fun saveSubagentToolsEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SUBAGENT_TOOLS_ENABLED] = enabled }
    }
    suspend fun saveSubagentDeleteEnabled(enabled: Boolean) { context.dataStore.edit { it[SUBAGENT_DELETE_ENABLED] = enabled } }
    suspend fun saveShellDevices(devices: List<ShellDeviceConfig>) {
        context.dataStore.edit { it[SHELL_DEVICES_JSON] = encryptShellDevices(devices, json) }
    }
    suspend fun saveMcpServers(servers: List<McpServerConfig>) {
        context.dataStore.edit {
            it[MCP_SERVERS_JSON] =
                com.newoether.agora.util.SecretCrypto.encrypt(json.encodeToString(servers))
        }
    }
    suspend fun saveSandboxEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SANDBOX_ENABLED] = enabled }
    }
    suspend fun saveSandboxSharedStorageEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SANDBOX_SHARED_STORAGE_ENABLED] = enabled }
    }
    suspend fun saveThemeMode(mode: String) {
        context.dataStore.edit { it[THEME_MODE] = mode }
    }
    suspend fun saveAmoledEnabled(enabled: Boolean) {
        context.dataStore.edit { it[AMOLED_ENABLED] = enabled }
    }
    suspend fun saveColorScheme(scheme: String) {
        context.dataStore.edit { it[COLOR_SCHEME] = scheme }
    }
    suspend fun saveDynamicColor(enabled: Boolean) {
        context.dataStore.edit { it[DYNAMIC_COLOR] = enabled }
    }
    suspend fun saveBlurEffectsEnabled(enabled: Boolean) {
        context.dataStore.edit { it[BLUR_EFFECTS_ENABLED] = enabled }
    }
    suspend fun saveReduceMotion(enabled: Boolean) {
        context.dataStore.edit { it[REDUCE_MOTION] = enabled }
    }
    suspend fun saveStickToBottom(enabled: Boolean) {
        context.dataStore.edit { it[STICK_TO_BOTTOM] = enabled }
    }
    suspend fun saveParseInlineDollarMath(enabled: Boolean) {
        context.dataStore.edit { it[PARSE_INLINE_DOLLAR_MATH] = enabled }
    }
    suspend fun saveAutoWrapCodeBlocks(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_WRAP_CODE_BLOCKS] = enabled }
    }
    suspend fun saveHapticsEnabled(enabled: Boolean) {
        context.dataStore.edit { it[HAPTICS_ENABLED] = enabled }
    }
    suspend fun saveDetailedTokenUsage(enabled: Boolean) {
        context.dataStore.edit { it[DETAILED_TOKEN_USAGE] = enabled }
    }
    suspend fun saveToolCallDisplayMode(mode: String) {
        context.dataStore.edit { it[TOOL_CALL_DISPLAY_MODE] = ToolCallDisplayModes.normalize(mode) }
    }
    suspend fun saveThinkingSegmentDisplayMode(mode: String) {
        context.dataStore.edit {
            it[THINKING_SEGMENT_DISPLAY_MODE] = ThinkingSegmentDisplayModes.normalize(mode)
        }
    }
    suspend fun saveAutoExpandActiveGroup(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_EXPAND_ACTIVE_GROUP] = enabled }
    }
    suspend fun saveFontPreference(value: String) {
        context.dataStore.edit { it[FONT_PREFERENCE] = value }
    }
    suspend fun saveCustomFontPath(value: String) {
        context.dataStore.edit { it[CUSTOM_FONT_PATH] = value }
    }
    suspend fun saveCustomFontName(value: String) {
        context.dataStore.edit { it[CUSTOM_FONT_NAME] = value }
    }
    suspend fun saveSchemeStyle(style: String) {
        context.dataStore.edit { it[SCHEME_STYLE] = style }
    }
    suspend fun saveFirstLaunchTime(time: Long) {
        context.dataStore.edit { it[FIRST_LAUNCH_TIME] = time }
    }
    suspend fun saveOnboardingCompleted(completed: Boolean) {
        context.dataStore.edit { it[ONBOARDING_COMPLETED] = completed }
    }
    suspend fun saveRatingPromptSubmitted(submitted: Boolean) {
        context.dataStore.edit { it[RATING_PROMPT_SUBMITTED] = submitted }
    }
    suspend fun saveRatingPromptDismissed(dismissed: Boolean) {
        context.dataStore.edit { it[RATING_PROMPT_DISMISSED] = dismissed }
    }
    suspend fun incrementMessagesSent() {
        context.dataStore.edit { it[TOTAL_MESSAGES_SENT] = (it[TOTAL_MESSAGES_SENT] ?: 0) + 1 }
    }

    suspend fun saveTorEnabled(enabled: Boolean) {
        context.dataStore.edit { it[TOR_ENABLED] = enabled }
    }

    suspend fun saveTorSocksPort(port: Int) {
        context.dataStore.edit { it[TOR_SOCKS_PORT] = port.coerceIn(1, 65535) }
    }
    // ── Auto Backup ───────────────────────────────────────────
    suspend fun saveAutoBackupEnabled(enabled: Boolean) = backupPreferenceStore.saveAutoBackupEnabled(enabled)
    suspend fun saveAutoBackupPeriodHours(hours: Int) = backupPreferenceStore.saveAutoBackupPeriodHours(hours)
    suspend fun saveAutoBackupCategories(categories: String) = backupPreferenceStore.saveAutoBackupCategories(categories)
    suspend fun saveAutoBackupDirectory(path: String) = backupPreferenceStore.saveAutoBackupDirectory(path)
    suspend fun saveAutoDeleteEnabled(enabled: Boolean) = backupPreferenceStore.saveAutoDeleteEnabled(enabled)
    suspend fun saveAutoDeletePeriodHours(hours: Int) = backupPreferenceStore.saveAutoDeletePeriodHours(hours)
    suspend fun saveLastBackupTimestamp(timestamp: Long) = backupPreferenceStore.saveLastBackupTimestamp(timestamp)
    suspend fun saveLastModelsFetchFingerprint(fingerprint: String) =
        modelPreferenceStore.saveLastModelsFetchFingerprint(fingerprint)

    /**
     * Clears only settings that are portable across devices. Secrets, conversation-scoped
     * overrides, local model files, sandbox state, onboarding/rating metadata, and auto-backup
     * configuration are deliberately outside this reset boundary.
     *
     * Composite portable records (remote embedding models, shell devices, MCP servers, and the
     * custom font) are rebuilt separately by the importer so device-local records and credentials
     * can be retained or cleared according to their own import category.
     */
    suspend fun resetPortableSettingsForImport() {
        context.dataStore.edit { prefs ->
            clearPortableSettings(prefs)
        }
    }
    suspend fun invalidatePortableModelCaches() =
        modelPreferenceStore.invalidatePortableModelCaches()
}
