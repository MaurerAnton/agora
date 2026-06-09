package com.newoether.agora.data

import android.content.Context
import com.newoether.agora.util.DebugLog
import androidx.datastore.preferences.core.booleanPreferencesKey
import androidx.datastore.preferences.core.edit
import androidx.datastore.preferences.core.intPreferencesKey
import androidx.datastore.preferences.core.longPreferencesKey
import androidx.datastore.preferences.core.stringPreferencesKey
import androidx.datastore.preferences.core.stringSetPreferencesKey
import androidx.datastore.preferences.preferencesDataStore
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.flow.map
import kotlinx.serialization.Serializable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.util.UUID

private val Context.dataStore by preferencesDataStore(name = "settings")

@Serializable
data class ApiKeyEntry(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val key: String,
    val provider: String = "Google"
)

@Serializable
data class CustomProviderConfig(
    val name: String
)

@Serializable
data class ShellDeviceConfig(
    val id: String = UUID.randomUUID().toString(),
    val name: String,
    val description: String = "",
    val serverUrl: String = "",
    val apiKey: String = "",
    val timeout: Int = 30,
    val conchPublicKey: String = ""
)

@Serializable
data class SystemPromptEntry(
    val id: String = UUID.randomUUID().toString(),
    val title: String,
    val content: String = "",
    val systemItems: List<PromptTemplateItem> = emptyList(),
    val userPrependItems: List<PromptTemplateItem> = emptyList(),
    val userPostpendItems: List<PromptTemplateItem> = emptyList()
) {
    val resolvedSystemItems: List<PromptTemplateItem>
        get() = if (systemItems.isNotEmpty()) systemItems
        else if (content.isNotBlank()) listOf(PromptTemplateItem(type = PromptItemType.CUSTOM, value = content))
        else emptyList()
}

@Serializable
data class ConversationSettings(
    val contextWindow: Int? = null,
    val temperature: Float? = null,
    val maxTokens: Int? = null,
    val topP: Float? = null,
    val frequencyPenalty: Float? = null,
    val presencePenalty: Float? = null,
    val codeExecutionEnabled: Boolean? = null,
    val googleSearchEnabled: Boolean? = null,
    val thinkingEnabled: Boolean? = null,
    val thinkingLevel: String? = null,
    val webSearchEnabled: Boolean? = null,
    val shellEnabled: Boolean? = null
) {
    fun isAllNull() = contextWindow == null && temperature == null && maxTokens == null && topP == null
        && frequencyPenalty == null && presencePenalty == null
        && codeExecutionEnabled == null && googleSearchEnabled == null && thinkingEnabled == null
        && thinkingLevel == null && webSearchEnabled == null && shellEnabled == null
}

class SettingsManager(private val context: Context) {
    private val json = Json { ignoreUnknownKeys = true }

    companion object {
        val SELECTED_MODEL = stringPreferencesKey("selected_model")
        val AVAILABLE_MODELS_JSON = stringPreferencesKey("available_models_json")
        val ENABLED_MODELS = stringSetPreferencesKey("enabled_models")
        
        val API_KEYS_JSON = stringPreferencesKey("api_keys_json")
        val ACTIVE_API_KEY_IDS_JSON = stringPreferencesKey("active_api_key_ids_json")
        
        val SYSTEM_PROMPTS_JSON = stringPreferencesKey("system_prompts_json")
        val ACTIVE_SYSTEM_PROMPT_ID = stringPreferencesKey("active_system_prompt_id")
        val MODEL_ALIASES_JSON = stringPreferencesKey("model_aliases_json")
        val MAX_CONTEXT_WINDOW = stringPreferencesKey("max_context_window")
        val VISUALIZE_CONTEXT_ROLLOUT = booleanPreferencesKey("visualize_context_rollout")
        val CODE_EXECUTION_ENABLED = booleanPreferencesKey("code_execution_enabled")
        val GOOGLE_SEARCH_ENABLED = booleanPreferencesKey("google_search_enabled")
        val THINKING_ENABLED = booleanPreferencesKey("thinking_enabled")
        val THINKING_LEVEL = stringPreferencesKey("thinking_level")
        val PROVIDER_BASE_URLS = stringPreferencesKey("provider_base_urls")
        val TITLE_GENERATION_ENABLED = booleanPreferencesKey("title_generation_enabled")
        val TITLE_GENERATION_MODEL = stringPreferencesKey("title_generation_model")
        val IMAGE_TRANSCRIPTION_ENABLED_MODELS = stringSetPreferencesKey("image_transcription_enabled_models")
        val IMAGE_TRANSCRIPTION_MODEL = stringPreferencesKey("image_transcription_model")
        val IMAGE_TRANSCRIPTION_BATCH_SIZE = intPreferencesKey("image_transcription_batch_size")
        val ACCESS_PAST_CONVERSATIONS = booleanPreferencesKey("access_past_conversations")
        val ACCESS_SAVED_MEMORIES = booleanPreferencesKey("access_saved_memories")
        val ACCESS_ACTIVE_MEMORY = booleanPreferencesKey("access_active_memory")
        val RAG_SEARCH_ENABLED = booleanPreferencesKey("rag_search_enabled")
        val MODEL_SEARCH_METHOD = stringPreferencesKey("model_search_method")
        val MANUAL_SEARCH_METHOD = stringPreferencesKey("manual_search_method")
        val EMBEDDING_MODELS_JSON = stringPreferencesKey("embedding_models_json")
        val ACTIVE_EMBEDDING_MODEL_ID = stringPreferencesKey("active_embedding_model_id")
        val APP_LANGUAGE = stringPreferencesKey("app_language")
        val WEB_SEARCH_ENABLED = booleanPreferencesKey("web_search_enabled")
        val WEB_SEARCH_PROVIDER = stringPreferencesKey("web_search_provider")
        val WEB_SEARCH_API_KEYS_JSON = stringPreferencesKey("web_search_api_keys_json")
        val WEB_SEARCH_NUM_RESULTS = intPreferencesKey("web_search_num_results")
        val WEB_SEARCH_BASE_URL = stringPreferencesKey("web_search_base_url")
        val SEARCH_CONTEXT_WINDOW = intPreferencesKey("search_context_window")
        val SEARCH_MATCH_LIMIT = intPreferencesKey("search_match_limit")
        val RAG_THRESHOLD = stringPreferencesKey("rag_threshold")
        val AUTO_CACHE_ENABLED = booleanPreferencesKey("auto_cache_enabled")
        val AUTO_UPDATE_CHECK = booleanPreferencesKey("auto_update_check")
        val LAST_UPDATE_CHECK_TIME = longPreferencesKey("last_update_check_time")
        val LOCAL_CHAT_MODELS_JSON = stringPreferencesKey("local_chat_models_json")
        val CUSTOM_PROVIDERS_JSON = stringPreferencesKey("custom_providers_json")
        val SHELL_ENABLED = booleanPreferencesKey("shell_enabled")
        val SHELL_DEVICES_JSON = stringPreferencesKey("shell_devices_json")
        val THEME_MODE = stringPreferencesKey("theme_mode")
        val COLOR_SCHEME = stringPreferencesKey("color_scheme")
        val DYNAMIC_COLOR = booleanPreferencesKey("dynamic_color")
        val SCHEME_STYLE = stringPreferencesKey("scheme_style")
        val FIRST_LAUNCH_TIME = longPreferencesKey("first_launch_time")
        val ONBOARDING_COMPLETED = booleanPreferencesKey("onboarding_completed")
        val RATING_PROMPT_SUBMITTED = booleanPreferencesKey("rating_prompt_submitted")
        val RATING_PROMPT_DISMISSED = booleanPreferencesKey("rating_prompt_dismissed")
        val SHOW_DOCUMENTATION_FAB = booleanPreferencesKey("show_documentation_fab")
        val TOTAL_MESSAGES_SENT = intPreferencesKey("total_messages_sent")
        val DEFAULT_TEMPERATURE = stringPreferencesKey("default_temperature")
        val DEFAULT_MAX_TOKENS = intPreferencesKey("default_max_tokens")
        val DEFAULT_TOP_P = stringPreferencesKey("default_top_p")
        val DEFAULT_FREQUENCY_PENALTY = stringPreferencesKey("default_frequency_penalty")
        val DEFAULT_PRESENCE_PENALTY = stringPreferencesKey("default_presence_penalty")
        val CONVERSATION_SETTINGS_JSON = stringPreferencesKey("conversation_settings_json")
        val TOR_ENABLED = booleanPreferencesKey("tor_enabled")
        val TOR_SOCKS_PORT = intPreferencesKey("tor_socks_port")
    }

    val selectedModel: Flow<String> = context.dataStore.data.map { it[SELECTED_MODEL] ?: "gemini-1.5-flash" }
    
    val providerBaseUrls: Flow<Map<String, String>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[PROVIDER_BASE_URLS] ?: "{}"
        try { json.decodeFromString<Map<String, String>>(jsonStr) } catch (e: Exception) { DebugLog.e("SettingsManager", "Failed to decode providerBaseUrls", e); emptyMap() }
    }

    val availableModels: Flow<Map<String, List<String>>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[AVAILABLE_MODELS_JSON] ?: "{}"
        try { json.decodeFromString<Map<String, List<String>>>(jsonStr) } catch (e: Exception) { DebugLog.e("SettingsManager", "Failed to decode availableModels", e); emptyMap() }
    }

    val enabledModels: Flow<Set<String>> = context.dataStore.data.map { it[ENABLED_MODELS] ?: emptySet() }

    val modelAliases: Flow<Map<String, String>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[MODEL_ALIASES_JSON] ?: "{}"
        try { json.decodeFromString<Map<String, String>>(jsonStr) } catch (e: Exception) { emptyMap() }
    }

    val apiKeys: Flow<List<ApiKeyEntry>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[API_KEYS_JSON] ?: "[]"
        try { json.decodeFromString<List<ApiKeyEntry>>(jsonStr) } catch (e: Exception) { emptyList() }
    }
    
    val activeApiKeyIds: Flow<Map<String, String>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[ACTIVE_API_KEY_IDS_JSON] ?: "{}"
        try { json.decodeFromString<Map<String, String>>(jsonStr) } catch (e: Exception) { emptyMap() }
    }

    val systemPrompts: Flow<List<SystemPromptEntry>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[SYSTEM_PROMPTS_JSON] ?: "[]"
        try { json.decodeFromString<List<SystemPromptEntry>>(jsonStr) } catch (e: Exception) { emptyList() }
    }
    
    val activeSystemPromptId: Flow<String?> = context.dataStore.data.map { it[ACTIVE_SYSTEM_PROMPT_ID] }

    val maxContextWindow: Flow<Int> = context.dataStore.data.map { it[MAX_CONTEXT_WINDOW]?.toIntOrNull() ?: 20 }
    val visualizeContextRollout: Flow<Boolean> = context.dataStore.data.map { it[VISUALIZE_CONTEXT_ROLLOUT] ?: false }
    val codeExecutionEnabled: Flow<Boolean> = context.dataStore.data.map { it[CODE_EXECUTION_ENABLED] ?: false }
    val googleSearchEnabled: Flow<Boolean> = context.dataStore.data.map { it[GOOGLE_SEARCH_ENABLED] ?: false }
    val thinkingEnabled: Flow<Boolean> = context.dataStore.data.map { it[THINKING_ENABLED] ?: true }
    val thinkingLevel: Flow<String> = context.dataStore.data.map { it[THINKING_LEVEL] ?: "medium" }

    val titleGenerationEnabled: Flow<Boolean> = context.dataStore.data.map { it[TITLE_GENERATION_ENABLED] ?: true }
    val titleGenerationModel: Flow<String?> = context.dataStore.data.map { it[TITLE_GENERATION_MODEL] }
    val imageTranscriptionEnabledModels: Flow<Set<String>> = context.dataStore.data.map { it[IMAGE_TRANSCRIPTION_ENABLED_MODELS] ?: emptySet() }
    val imageTranscriptionModel: Flow<String?> = context.dataStore.data.map { it[IMAGE_TRANSCRIPTION_MODEL] }
    val imageTranscriptionBatchSize: Flow<Int> = context.dataStore.data.map { it[IMAGE_TRANSCRIPTION_BATCH_SIZE] ?: 3 }

    val accessPastConversations: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_PAST_CONVERSATIONS] ?: true }
    val accessSavedMemories: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_SAVED_MEMORIES] ?: true }
    val accessActiveMemory: Flow<Boolean> = context.dataStore.data.map { it[ACCESS_ACTIVE_MEMORY] ?: true }
    val ragSearchEnabled: Flow<Boolean> = context.dataStore.data.map { it[RAG_SEARCH_ENABLED] ?: false }
    val modelSearchMethod: Flow<String> = context.dataStore.data.map { it[MODEL_SEARCH_METHOD] ?: "keyword" }
    val manualSearchMethod: Flow<String> = context.dataStore.data.map { it[MANUAL_SEARCH_METHOD] ?: "keyword" }
    val embeddingModels: Flow<List<EmbeddingModelConfig>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[EMBEDDING_MODELS_JSON] ?: "[]"
        try { json.decodeFromString<List<EmbeddingModelConfig>>(jsonStr) } catch (e: Exception) { emptyList() }
    }
    val activeEmbeddingModelId: Flow<String> = context.dataStore.data.map { it[ACTIVE_EMBEDDING_MODEL_ID] ?: "" }

    val appLanguage: Flow<String> = context.dataStore.data.map { it[APP_LANGUAGE] ?: "system" }
    val webSearchEnabled: Flow<Boolean> = context.dataStore.data.map { it[WEB_SEARCH_ENABLED] ?: true }
    val webSearchProvider: Flow<String> = context.dataStore.data.map { it[WEB_SEARCH_PROVIDER] ?: "brave" }
    val webSearchApiKeys: Flow<Map<String, String>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[WEB_SEARCH_API_KEYS_JSON] ?: "{}"
        try { json.decodeFromString<Map<String, String>>(jsonStr) } catch (e: Exception) { DebugLog.e("SettingsManager", "Failed to decode webSearchApiKeys", e); emptyMap() }
    }
    val webSearchNumResults: Flow<Int> = context.dataStore.data.map { it[WEB_SEARCH_NUM_RESULTS] ?: 5 }
    val webSearchBaseUrl: Flow<String> = context.dataStore.data.map { it[WEB_SEARCH_BASE_URL] ?: "" }
    val searchContextWindow: Flow<Int> = context.dataStore.data.map { it[SEARCH_CONTEXT_WINDOW] ?: 8 }
    val searchMatchLimit: Flow<Int> = context.dataStore.data.map { it[SEARCH_MATCH_LIMIT] ?: 10 }
    val ragThreshold: Flow<Float> = context.dataStore.data.map { it[RAG_THRESHOLD]?.toFloatOrNull() ?: 0.5f }
    val defaultTemperature: Flow<Float?> = context.dataStore.data.map { it[DEFAULT_TEMPERATURE]?.toFloatOrNull() }
    val defaultMaxTokens: Flow<Int?> = context.dataStore.data.map { it[DEFAULT_MAX_TOKENS] }
    val defaultTopP: Flow<Float?> = context.dataStore.data.map { it[DEFAULT_TOP_P]?.toFloatOrNull() }
    val defaultFrequencyPenalty: Flow<Float?> = context.dataStore.data.map { it[DEFAULT_FREQUENCY_PENALTY]?.toFloatOrNull() }
    val defaultPresencePenalty: Flow<Float?> = context.dataStore.data.map { it[DEFAULT_PRESENCE_PENALTY]?.toFloatOrNull() }
    val conversationSettings: Flow<Map<String, ConversationSettings>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[CONVERSATION_SETTINGS_JSON] ?: "{}"
        try { json.decodeFromString<Map<String, ConversationSettings>>(jsonStr) } catch (e: Exception) { emptyMap() }
    }
    val autoCacheEnabled: Flow<Boolean> = context.dataStore.data.map { it[AUTO_CACHE_ENABLED] ?: true }
    val autoUpdateCheck: Flow<Boolean> = context.dataStore.data.map { it[AUTO_UPDATE_CHECK] ?: true }
    val lastUpdateCheckTime: Flow<Long> = context.dataStore.data.map { it[LAST_UPDATE_CHECK_TIME] ?: 0L }
    val localChatModels: Flow<List<LocalChatModelConfig>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[LOCAL_CHAT_MODELS_JSON] ?: "[]"
        try { json.decodeFromString<List<LocalChatModelConfig>>(jsonStr) } catch (e: Exception) { emptyList() }
    }
    val customProviders: Flow<List<CustomProviderConfig>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[CUSTOM_PROVIDERS_JSON] ?: "[]"
        try { json.decodeFromString<List<CustomProviderConfig>>(jsonStr) } catch (e: Exception) { emptyList() }
    }

    val showDocumentationFab: Flow<Boolean> = context.dataStore.data.map { it[SHOW_DOCUMENTATION_FAB] ?: true }

    val shellEnabled: Flow<Boolean> = context.dataStore.data.map { it[SHELL_ENABLED] ?: true }
    val shellDevices: Flow<List<ShellDeviceConfig>> = context.dataStore.data.map { pref ->
        val jsonStr = pref[SHELL_DEVICES_JSON] ?: "[]"
        try { json.decodeFromString<List<ShellDeviceConfig>>(jsonStr) } catch (e: Exception) { emptyList() }
    }

    val themeMode: Flow<String> = context.dataStore.data.map { it[THEME_MODE] ?: "FOLLOW_DEVICE" }
    val colorScheme: Flow<String> = context.dataStore.data.map { it[COLOR_SCHEME] ?: "DEFAULT" }
    val dynamicColor: Flow<Boolean> = context.dataStore.data.map { it[DYNAMIC_COLOR] ?: true }
    val schemeStyle: Flow<String> = context.dataStore.data.map { it[SCHEME_STYLE] ?: "TONAL_SPOT" }
    val firstLaunchTime: Flow<Long?> = context.dataStore.data.map { it[FIRST_LAUNCH_TIME] }
    val onboardingCompleted: Flow<Boolean> = context.dataStore.data.map { it[ONBOARDING_COMPLETED] ?: false }
    val ratingPromptSubmitted: Flow<Boolean> = context.dataStore.data.map { it[RATING_PROMPT_SUBMITTED] ?: false }
    val ratingPromptDismissed: Flow<Boolean> = context.dataStore.data.map { it[RATING_PROMPT_DISMISSED] ?: false }
    val totalMessagesSent: Flow<Int> = context.dataStore.data.map { it[TOTAL_MESSAGES_SENT] ?: 0 }

    val torEnabled: Flow<Boolean> = context.dataStore.data.map { it[TOR_ENABLED] ?: false }
    val torSocksPort: Flow<Int> = context.dataStore.data.map { it[TOR_SOCKS_PORT] ?: 9050 }

    suspend fun saveProviderBaseUrl(provider: String, url: String) {
        context.dataStore.edit { prefs ->
            val current = prefs[PROVIDER_BASE_URLS] ?: "{}"
            val map = try { json.decodeFromString<MutableMap<String, String>>(current) } catch (e: Exception) { mutableMapOf() }
            map[provider] = url
            prefs[PROVIDER_BASE_URLS] = json.encodeToString(map)
        }
    }

    suspend fun saveSelectedModel(model: String) {
        context.dataStore.edit { it[SELECTED_MODEL] = model }
    }

    suspend fun saveAvailableModels(provider: String, models: List<String>) {
        context.dataStore.edit { prefs ->
            val current = prefs[AVAILABLE_MODELS_JSON] ?: "{}"
            val map = try { json.decodeFromString<MutableMap<String, List<String>>>(current) } catch (e: Exception) { mutableMapOf() }
            map[provider] = models
            prefs[AVAILABLE_MODELS_JSON] = json.encodeToString(map)
        }
    }

    suspend fun saveEnabledModels(models: Set<String>) {
        context.dataStore.edit { it[ENABLED_MODELS] = models }
    }

    suspend fun saveModelAliases(aliases: Map<String, String>) {
        context.dataStore.edit { it[MODEL_ALIASES_JSON] = json.encodeToString(aliases) }
    }

    suspend fun saveApiKeys(keys: List<ApiKeyEntry>) {
        context.dataStore.edit { it[API_KEYS_JSON] = json.encodeToString(keys) }
    }

    suspend fun setActiveApiKeyId(provider: String, id: String?) {
        context.dataStore.edit { prefs ->
            val current = prefs[ACTIVE_API_KEY_IDS_JSON] ?: "{}"
            val map = try { json.decodeFromString<MutableMap<String, String>>(current) } catch (e: Exception) { mutableMapOf() }
            if (id == null) map.remove(provider) else map[provider] = id
            prefs[ACTIVE_API_KEY_IDS_JSON] = json.encodeToString(map)
        }
    }

    suspend fun saveSystemPrompts(prompts: List<SystemPromptEntry>) {
        context.dataStore.edit { it[SYSTEM_PROMPTS_JSON] = json.encodeToString(prompts) }
    }

    suspend fun setActiveSystemPromptId(id: String?) {
        context.dataStore.edit { 
            if (id == null) it.remove(ACTIVE_SYSTEM_PROMPT_ID) else it[ACTIVE_SYSTEM_PROMPT_ID] = id 
        }
    }

    suspend fun saveMaxContextWindow(window: Int) {
        context.dataStore.edit { it[MAX_CONTEXT_WINDOW] = window.toString() }
    }

    suspend fun saveVisualizeContextRollout(enabled: Boolean) {
        context.dataStore.edit { it[VISUALIZE_CONTEXT_ROLLOUT] = enabled }
    }

    suspend fun saveCodeExecutionEnabled(enabled: Boolean) {
        context.dataStore.edit { it[CODE_EXECUTION_ENABLED] = enabled }
    }

    suspend fun saveGoogleSearchEnabled(enabled: Boolean) {
        context.dataStore.edit { it[GOOGLE_SEARCH_ENABLED] = enabled }
    }

    suspend fun saveThinkingEnabled(enabled: Boolean) {
        context.dataStore.edit { it[THINKING_ENABLED] = enabled }
    }

    suspend fun saveThinkingLevel(level: String) {
        context.dataStore.edit { it[THINKING_LEVEL] = level }
    }

    suspend fun saveTitleGenerationEnabled(enabled: Boolean) {
        context.dataStore.edit { it[TITLE_GENERATION_ENABLED] = enabled }
    }

    suspend fun saveAccessPastConversations(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_PAST_CONVERSATIONS] = enabled }
    }

    suspend fun saveAccessSavedMemories(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_SAVED_MEMORIES] = enabled }
    }
    suspend fun saveAccessActiveMemory(enabled: Boolean) {
        context.dataStore.edit { it[ACCESS_ACTIVE_MEMORY] = enabled }
    }
    suspend fun saveRagSearchEnabled(enabled: Boolean) {
        context.dataStore.edit { it[RAG_SEARCH_ENABLED] = enabled }
    }
    suspend fun saveAutoCacheEnabled(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_CACHE_ENABLED] = enabled }
    }
    suspend fun saveAutoUpdateCheck(enabled: Boolean) {
        context.dataStore.edit { it[AUTO_UPDATE_CHECK] = enabled }
    }
    suspend fun saveLastUpdateCheckTime(time: Long) {
        context.dataStore.edit { it[LAST_UPDATE_CHECK_TIME] = time }
    }
    suspend fun saveModelSearchMethod(method: String) {
        context.dataStore.edit { it[MODEL_SEARCH_METHOD] = method }
    }
    suspend fun saveManualSearchMethod(method: String) {
        context.dataStore.edit { it[MANUAL_SEARCH_METHOD] = method }
    }
    suspend fun saveEmbeddingModels(models: List<EmbeddingModelConfig>) {
        context.dataStore.edit { it[EMBEDDING_MODELS_JSON] = json.encodeToString(models) }
    }
    suspend fun setActiveEmbeddingModelId(id: String) {
        context.dataStore.edit { it[ACTIVE_EMBEDDING_MODEL_ID] = id }
    }
    suspend fun saveAppLanguage(language: String) {
        context.dataStore.edit { it[APP_LANGUAGE] = language }
    }

    suspend fun saveWebSearchEnabled(enabled: Boolean) {
        context.dataStore.edit { it[WEB_SEARCH_ENABLED] = enabled }
    }

    suspend fun saveWebSearchProvider(provider: String) {
        context.dataStore.edit { it[WEB_SEARCH_PROVIDER] = provider }
    }

    suspend fun saveWebSearchApiKey(provider: String, apiKey: String) {
        context.dataStore.edit { prefs ->
            val current = prefs[WEB_SEARCH_API_KEYS_JSON] ?: "{}"
            val map = try { json.decodeFromString<MutableMap<String, String>>(current) } catch (e: Exception) { mutableMapOf() }
            if (apiKey.isBlank()) map.remove(provider) else map[provider] = apiKey
            prefs[WEB_SEARCH_API_KEYS_JSON] = json.encodeToString(map)
        }
    }

    suspend fun saveWebSearchNumResults(n: Int) {
        context.dataStore.edit { it[WEB_SEARCH_NUM_RESULTS] = n.coerceIn(1, 10) }
    }
    suspend fun saveWebSearchBaseUrl(url: String) {
        context.dataStore.edit { it[WEB_SEARCH_BASE_URL] = url }
    }
    suspend fun saveSearchMatchLimit(n: Int) {
        context.dataStore.edit { it[SEARCH_MATCH_LIMIT] = n }
    }
    suspend fun saveSearchContextWindow(n: Int) {
        context.dataStore.edit { it[SEARCH_CONTEXT_WINDOW] = n }
    }
    suspend fun saveRagThreshold(threshold: Float) {
        context.dataStore.edit { it[RAG_THRESHOLD] = threshold.toString() }
    }
    suspend fun saveDefaultTemperature(value: Float?) {
        context.dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_TEMPERATURE) else prefs[DEFAULT_TEMPERATURE] = value.toString()
        }
    }
    suspend fun saveDefaultMaxTokens(value: Int?) {
        context.dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_MAX_TOKENS) else prefs[DEFAULT_MAX_TOKENS] = value
        }
    }
    suspend fun saveDefaultTopP(value: Float?) {
        context.dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_TOP_P) else prefs[DEFAULT_TOP_P] = value.toString()
        }
    }
    suspend fun saveDefaultFrequencyPenalty(value: Float?) {
        context.dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_FREQUENCY_PENALTY) else prefs[DEFAULT_FREQUENCY_PENALTY] = value.toString()
        }
    }
    suspend fun saveDefaultPresencePenalty(value: Float?) {
        context.dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_PRESENCE_PENALTY) else prefs[DEFAULT_PRESENCE_PENALTY] = value.toString()
        }
    }
    suspend fun saveConversationSettings(conversationId: String, settings: ConversationSettings?) {
        context.dataStore.edit { prefs ->
            val current = prefs[CONVERSATION_SETTINGS_JSON] ?: "{}"
            val map = try { json.decodeFromString<MutableMap<String, ConversationSettings>>(current) } catch (e: Exception) { mutableMapOf() }
            if (settings == null || settings.isAllNull()) map.remove(conversationId)
            else map[conversationId] = settings
            prefs[CONVERSATION_SETTINGS_JSON] = json.encodeToString(map)
        }
    }

    suspend fun saveLocalChatModels(models: List<LocalChatModelConfig>) {
        context.dataStore.edit { it[LOCAL_CHAT_MODELS_JSON] = json.encodeToString(models) }
    }
    suspend fun saveCustomProviders(providers: List<CustomProviderConfig>) {
        context.dataStore.edit { it[CUSTOM_PROVIDERS_JSON] = json.encodeToString(providers) }
    }

    suspend fun saveTitleGenerationModel(model: String?) {
        context.dataStore.edit {
            if (model == null) it.remove(TITLE_GENERATION_MODEL)
            else it[TITLE_GENERATION_MODEL] = model
        }
    }

    suspend fun saveImageTranscriptionEnabledModels(models: Set<String>) {
        context.dataStore.edit { it[IMAGE_TRANSCRIPTION_ENABLED_MODELS] = models }
    }

    suspend fun saveImageTranscriptionModel(model: String?) {
        context.dataStore.edit {
            if (model == null) it.remove(IMAGE_TRANSCRIPTION_MODEL)
            else it[IMAGE_TRANSCRIPTION_MODEL] = model
        }
    }

    suspend fun saveImageTranscriptionBatchSize(size: Int) {
        context.dataStore.edit { it[IMAGE_TRANSCRIPTION_BATCH_SIZE] = size.coerceIn(1, 10) }
    }

    suspend fun saveShowDocumentationFab(enabled: Boolean) {
        context.dataStore.edit { it[SHOW_DOCUMENTATION_FAB] = enabled }
    }

    suspend fun saveShellEnabled(enabled: Boolean) {
        context.dataStore.edit { it[SHELL_ENABLED] = enabled }
    }

    suspend fun saveShellDevices(devices: List<ShellDeviceConfig>) {
        context.dataStore.edit { it[SHELL_DEVICES_JSON] = json.encodeToString(devices) }
    }

    suspend fun saveThemeMode(mode: String) {
        context.dataStore.edit { it[THEME_MODE] = mode }
    }
    suspend fun saveColorScheme(scheme: String) {
        context.dataStore.edit { it[COLOR_SCHEME] = scheme }
    }
    suspend fun saveDynamicColor(enabled: Boolean) {
        context.dataStore.edit { it[DYNAMIC_COLOR] = enabled }
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
}
