package com.newoether.agora.data

import androidx.datastore.core.DataStore
import androidx.datastore.preferences.core.Preferences
import androidx.datastore.preferences.core.edit
import com.newoether.agora.model.ContextBudget
import com.newoether.agora.model.OpenAiServiceTiers
import com.newoether.agora.model.ThinkingLevels
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.map
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json

/**
 * Generation, retrieval and per-request parameters previously held directly by [SettingsManager].
 *
 * The bodies live here so `SettingsManager.kt` stays within the repository 800-line source
 * budget (see `development/kotlin-source-size-policy.md`). Every member keeps its original
 * name, signature and behaviour; [SettingsManager] only delegates to this store.
 */
internal class SettingsGenerationParametersStore(
    private val dataStore: DataStore<Preferences>,
    private val json: Json,
) {
    val maxContextWindow: Flow<Int> = dataStore.data.map { preferences ->
        ContextBudget.normalize(
            preferences[CONTEXT_TOKEN_BUDGET]?.toIntOrNull()
                ?: preferences[MAX_CONTEXT_WINDOW]?.toIntOrNull()
        )
    }
    val visualizeContextRollout: Flow<Boolean> = dataStore.data.map { it[VISUALIZE_CONTEXT_ROLLOUT] ?: false }
    val contextCompactEnabled: Flow<Boolean> = dataStore.data.map {
        it[CONTEXT_COMPACT_ENABLED] ?: DEFAULT_CONTEXT_COMPACT_ENABLED
    }
    val contextCompactModel: Flow<String?> = dataStore.data.map { it[CONTEXT_COMPACT_MODEL] }
    val contextCompactPrompt: Flow<String> = dataStore.data.map { pref ->
        pref[CONTEXT_COMPACT_PROMPT]?.takeIf { it.isNotBlank() } ?: BuiltInPrompts.CONTEXT_COMPACT_SYSTEM
    }
    val contextCompactRetainCount: Flow<Int> = dataStore.data.map {
        it[CONTEXT_COMPACT_RETAIN_COUNT] ?: DEFAULT_CONTEXT_COMPACT_RETAIN_COUNT
    }
    val contextCompactPreserveSystemPrompt: Flow<Boolean> = dataStore.data.map {
        it[CONTEXT_COMPACT_PRESERVE_SYSTEM_PROMPT]
            ?: DEFAULT_CONTEXT_COMPACT_PRESERVE_SYSTEM_PROMPT
    }
    val contextCompactThresholdPercent: Flow<Int> = dataStore.data.map {
        it[CONTEXT_COMPACT_THRESHOLD_PERCENT]
            ?.takeIf(CONTEXT_COMPACT_THRESHOLD_PERCENT_RANGE::contains)
            ?: DEFAULT_CONTEXT_COMPACT_THRESHOLD_PERCENT
    }
    val codeExecutionEnabled: Flow<Boolean> = dataStore.data.map { it[CODE_EXECUTION_ENABLED] ?: false }
    val googleSearchEnabled: Flow<Boolean> = dataStore.data.map { it[GOOGLE_SEARCH_ENABLED] ?: false }
    val thinkingEnabled: Flow<Boolean> = dataStore.data.map { it[THINKING_ENABLED] ?: true }
    val autoRegenOnCjk: Flow<Boolean> = dataStore.data.map { it[AUTO_REGEN_ON_CJK] ?: true }
    val maxCjkRetries: Flow<Int> = dataStore.data.map { it[MAX_CJK_AUTO_RETRIES]?.coerceIn(0, 99) ?: 2 }
    val thinkingLevel: Flow<String> = dataStore.data.map { ThinkingLevels.normalize(it[THINKING_LEVEL]) }
    val thinkingBudgetEnabled: Flow<Boolean> = dataStore.data.map { pref ->
        pref[THINKING_BUDGET_ENABLED] ?: (ThinkingLevels.legacyBudgetTokens(pref[THINKING_LEVEL]) != null)
    }
    val thinkingBudgetTokens: Flow<Int> = dataStore.data.map { pref ->
        pref[THINKING_BUDGET_TOKENS]
            ?: ThinkingLevels.legacyBudgetTokens(pref[THINKING_LEVEL])
            ?: ThinkingLevels.DefaultBudgetTokens
    }
    val openAiServiceTierEnabled: Flow<Boolean> =
        dataStore.data.map { it[OPENAI_SERVICE_TIER_ENABLED] ?: false }
    val openAiServiceTier: Flow<String> = dataStore.data.map { pref ->
        OpenAiServiceTiers.normalize(pref[OPENAI_SERVICE_TIER])
    }
    val openAiResponsesApiEnabled: Flow<Boolean> =
        dataStore.data.map { it[OPENAI_RESPONSES_API_ENABLED] ?: false }
    val titleGenerationEnabled: Flow<Boolean> = dataStore.data.map { it[TITLE_GENERATION_ENABLED] ?: true }
    val titleGenerationModel: Flow<String?> = dataStore.data.map { it[TITLE_GENERATION_MODEL] }
    val titleGenerationPrompt: Flow<String> = dataStore.data.map { pref ->
        pref[TITLE_GENERATION_PROMPT]?.takeIf { it.isNotBlank() } ?: BuiltInPrompts.TITLE_GENERATION_SYSTEM
    }
    val titleGenerationNotificationsEnabled: Flow<Boolean> = dataStore.data.map {
        it[TITLE_GENERATION_NOTIFICATIONS_ENABLED] ?: true
    }
    val imageTranscriptionEnabled: Flow<Boolean> = dataStore.data.map {
        it[IMAGE_TRANSCRIPTION_ENABLED] ?: true
    }
    val imageTranscriptionEnabledModels: Flow<Set<String>> = dataStore.data.map { it[IMAGE_TRANSCRIPTION_ENABLED_MODELS] ?: emptySet() }
    val imageTranscriptionModel: Flow<String?> = dataStore.data.map { it[IMAGE_TRANSCRIPTION_MODEL] }
    val imageTranscriptionBatchSize: Flow<Int> = dataStore.data.map { it[IMAGE_TRANSCRIPTION_BATCH_SIZE] ?: 3 }
    val imageTranscriptionPrompt: Flow<String> = dataStore.data.map { pref ->
        pref[IMAGE_TRANSCRIPTION_PROMPT]?.takeIf { it.isNotBlank() } ?: BuiltInPrompts.IMAGE_TRANSCRIPTION_USER
    }

    val ragSearchEnabled: Flow<Boolean> = dataStore.data.map { it[RAG_SEARCH_ENABLED] ?: false }
    val modelSearchMethod: Flow<String> = dataStore.data.map { it[MODEL_SEARCH_METHOD] ?: "keyword" }
    val manualSearchMethod: Flow<String> = dataStore.data.map { it[MANUAL_SEARCH_METHOD] ?: "keyword" }
    val embeddingModels: Flow<List<EmbeddingModelConfig>> = dataStore.data.map { pref ->
        val jsonStr = pref[EMBEDDING_MODELS_JSON] ?: "[]"
        try { json.decodeFromString<List<EmbeddingModelConfig>>(jsonStr) } catch (e: Exception) { emptyList() }
    }
    val activeEmbeddingModelId: Flow<String> = dataStore.data.map { it[ACTIVE_EMBEDDING_MODEL_ID] ?: "" }

    val webSearchEnabled: Flow<Boolean> = dataStore.data.map { it[WEB_SEARCH_ENABLED] ?: true }
    val webSearchProvider: Flow<String> = dataStore.data.map {
        normalizeWebSearchProvider(it[WEB_SEARCH_PROVIDER])
    }
    val webSearchApiKeys: Flow<Map<String, String>> = dataStore.data.map { preferences ->
        decodeWebSearchApiKeys(preferences, json)
    }
    val webSearchNumResults: Flow<Int> = dataStore.data.map { it[WEB_SEARCH_NUM_RESULTS] ?: 5 }
    val webSearchBaseUrl: Flow<String> = dataStore.data.map { it[WEB_SEARCH_BASE_URL] ?: "" }

    val imageGenEnabled: Flow<Boolean> = dataStore.data.map { it[IMAGE_GEN_ENABLED] ?: false }
    // Selected image model "Provider:modelId" (null = none chosen). Creds reused from that provider.
    val imageGenModel: Flow<String?> = dataStore.data.map { it[IMAGE_GEN_MODEL] }
    val imageGenSize: Flow<String> = dataStore.data.map { it[IMAGE_GEN_SIZE] ?: "1024x1024" }
    val searchContextWindow: Flow<Int> = dataStore.data.map { it[SEARCH_CONTEXT_WINDOW] ?: 8 }
    val searchMatchLimit: Flow<Int> = dataStore.data.map { it[SEARCH_MATCH_LIMIT] ?: 10 }
    val ragThreshold: Flow<Float> = dataStore.data.map { it[RAG_THRESHOLD]?.toFloatOrNull() ?: 0.5f }
    val defaultTemperature: Flow<Float?> = dataStore.data.map { it[DEFAULT_TEMPERATURE]?.toFloatOrNull() }
    val defaultMaxTokens: Flow<Int?> = dataStore.data.map { it[DEFAULT_MAX_TOKENS] }
    val defaultTopP: Flow<Float?> = dataStore.data.map { it[DEFAULT_TOP_P]?.toFloatOrNull() }
    val defaultFrequencyPenalty: Flow<Float?> = dataStore.data.map { it[DEFAULT_FREQUENCY_PENALTY]?.toFloatOrNull() }
    val defaultPresencePenalty: Flow<Float?> = dataStore.data.map { it[DEFAULT_PRESENCE_PENALTY]?.toFloatOrNull() }
    val conversationSettings: Flow<Map<String, ConversationSettings>> =
        dataStore.data.map { preferences -> decodeConversationSettings(preferences, json) }

    suspend fun saveMaxContextWindow(window: Int) {
        dataStore.edit {
            it[CONTEXT_TOKEN_BUDGET] = ContextBudget.normalize(window).toString()
        }
    }
    suspend fun saveVisualizeContextRollout(enabled: Boolean) {
        dataStore.edit { it[VISUALIZE_CONTEXT_ROLLOUT] = enabled }
    }
    suspend fun saveCodeExecutionEnabled(enabled: Boolean) {
        dataStore.edit { it[CODE_EXECUTION_ENABLED] = enabled }
    }
    suspend fun saveGoogleSearchEnabled(enabled: Boolean) {
        dataStore.edit { it[GOOGLE_SEARCH_ENABLED] = enabled }
    }
    suspend fun saveThinkingEnabled(enabled: Boolean) {
        dataStore.edit { it[THINKING_ENABLED] = enabled }
    }
    suspend fun saveAutoRegenOnCjk(enabled: Boolean) {
        dataStore.edit { it[AUTO_REGEN_ON_CJK] = enabled }
    }
    suspend fun saveMaxCjkRetries(retries: Int) {
        dataStore.edit { it[MAX_CJK_AUTO_RETRIES] = retries.coerceIn(0, 99) }
    }
    suspend fun saveThinkingLevel(level: String) {
        dataStore.edit { it[THINKING_LEVEL] = ThinkingLevels.normalize(level) }
    }
    suspend fun saveThinkingBudgetEnabled(enabled: Boolean) {
        dataStore.edit { it[THINKING_BUDGET_ENABLED] = enabled }
    }
    suspend fun saveThinkingBudgetTokens(tokens: Int) {
        dataStore.edit { it[THINKING_BUDGET_TOKENS] = tokens.coerceAtLeast(1) }
    }
    suspend fun saveOpenAiServiceTierEnabled(enabled: Boolean) {
        dataStore.edit { it[OPENAI_SERVICE_TIER_ENABLED] = enabled }
    }
    suspend fun saveOpenAiServiceTier(tier: String) {
        dataStore.edit {
            it[OPENAI_SERVICE_TIER] = OpenAiServiceTiers.normalize(tier)
        }
    }
    suspend fun saveOpenAiResponsesApiEnabled(enabled: Boolean) {
        dataStore.edit { it[OPENAI_RESPONSES_API_ENABLED] = enabled }
    }
    suspend fun saveTitleGenerationEnabled(enabled: Boolean) {
        dataStore.edit { it[TITLE_GENERATION_ENABLED] = enabled }
    }
    suspend fun saveTitleGenerationNotificationsEnabled(enabled: Boolean) {
        dataStore.edit { it[TITLE_GENERATION_NOTIFICATIONS_ENABLED] = enabled }
    }
    suspend fun saveRagSearchEnabled(enabled: Boolean) {
        dataStore.edit { it[RAG_SEARCH_ENABLED] = enabled }
    }
    suspend fun saveModelSearchMethod(method: String) {
        dataStore.edit { it[MODEL_SEARCH_METHOD] = method }
    }
    suspend fun saveManualSearchMethod(method: String) {
        dataStore.edit { it[MANUAL_SEARCH_METHOD] = method }
    }
    suspend fun saveEmbeddingModels(models: List<EmbeddingModelConfig>) {
        dataStore.edit { it[EMBEDDING_MODELS_JSON] = json.encodeToString(models) }
    }
    suspend fun setActiveEmbeddingModelId(id: String) {
        dataStore.edit { it[ACTIVE_EMBEDDING_MODEL_ID] = id }
    }
    suspend fun saveWebSearchEnabled(enabled: Boolean) {
        dataStore.edit { it[WEB_SEARCH_ENABLED] = enabled }
    }
    suspend fun saveWebSearchProvider(provider: String) {
        dataStore.edit { it[WEB_SEARCH_PROVIDER] = normalizeWebSearchProvider(provider) }
    }
    suspend fun saveWebSearchApiKey(provider: String, apiKey: String) {
        dataStore.edit { prefs ->
            val current = com.newoether.agora.util.SecretCrypto.decrypt(prefs[WEB_SEARCH_API_KEYS_JSON] ?: "{}")
            val map = try { json.decodeFromString<MutableMap<String, String>>(current) } catch (e: Exception) { mutableMapOf() }
            if (apiKey.isBlank()) map.remove(provider) else map[provider] = apiKey
            prefs[WEB_SEARCH_API_KEYS_JSON] = com.newoether.agora.util.SecretCrypto.encrypt(json.encodeToString(map))
        }
    }
    suspend fun saveWebSearchApiKeys(keys: Map<String, String>) {
        val nonBlank = keys.filterValues { it.isNotBlank() }
        dataStore.edit { prefs ->
            if (nonBlank.isEmpty()) {
                prefs.remove(WEB_SEARCH_API_KEYS_JSON)
            } else {
                prefs[WEB_SEARCH_API_KEYS_JSON] =
                    com.newoether.agora.util.SecretCrypto.encrypt(json.encodeToString(nonBlank))
            }
        }
    }
    suspend fun saveWebSearchNumResults(n: Int) {
        dataStore.edit { it[WEB_SEARCH_NUM_RESULTS] = n.coerceIn(1, 10) }
    }
    suspend fun saveWebSearchBaseUrl(url: String) {
        dataStore.edit { it[WEB_SEARCH_BASE_URL] = url }
    }
    suspend fun saveImageGenEnabled(enabled: Boolean) {
        dataStore.edit { it[IMAGE_GEN_ENABLED] = enabled }
    }
    suspend fun saveImageGenModel(model: String?) {
        dataStore.edit {
            if (model == null) it.remove(IMAGE_GEN_MODEL) else it[IMAGE_GEN_MODEL] = model
        }
    }
    suspend fun saveImageGenSize(size: String) {
        dataStore.edit { it[IMAGE_GEN_SIZE] = size }
    }
    suspend fun saveSearchMatchLimit(n: Int) {
        dataStore.edit { it[SEARCH_MATCH_LIMIT] = n }
    }
    suspend fun saveSearchContextWindow(n: Int) {
        dataStore.edit { it[SEARCH_CONTEXT_WINDOW] = n }
    }
    suspend fun saveRagThreshold(threshold: Float) {
        dataStore.edit { it[RAG_THRESHOLD] = threshold.toString() }
    }
    suspend fun saveDefaultTemperature(value: Float?) {
        dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_TEMPERATURE) else prefs[DEFAULT_TEMPERATURE] = value.toString()
        }
    }
    suspend fun saveDefaultMaxTokens(value: Int?) {
        dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_MAX_TOKENS) else prefs[DEFAULT_MAX_TOKENS] = value
        }
    }
    suspend fun saveDefaultTopP(value: Float?) {
        dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_TOP_P) else prefs[DEFAULT_TOP_P] = value.toString()
        }
    }
    suspend fun saveDefaultFrequencyPenalty(value: Float?) {
        dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_FREQUENCY_PENALTY) else prefs[DEFAULT_FREQUENCY_PENALTY] = value.toString()
        }
    }
    suspend fun saveDefaultPresencePenalty(value: Float?) {
        dataStore.edit { prefs ->
            if (value == null) prefs.remove(DEFAULT_PRESENCE_PENALTY) else prefs[DEFAULT_PRESENCE_PENALTY] = value.toString()
        }
    }
    suspend fun saveConversationSettings(
        conversationId: String,
        settings: ConversationSettings?,
    ): Map<String, ConversationSettings> {
        var updated: Map<String, ConversationSettings> = emptyMap()
        dataStore.edit { prefs ->
            val map = decodeConversationSettings(prefs, json).toMutableMap()
            if (settings == null || settings.isAllNull()) map.remove(conversationId)
            else map[conversationId] = settings.normalizedServiceTier()
            updated = map.toMap()
            prefs[CONVERSATION_SETTINGS_JSON] = json.encodeToString(map)
        }
        return updated
    }
    suspend fun saveConversationSettingsMap(settings: Map<String, ConversationSettings>) {
        val nonEmpty = settings.filterValues { !it.isAllNull() }
            .mapValues { (_, value) -> value.normalizedServiceTier() }
        dataStore.edit { prefs ->
            if (nonEmpty.isEmpty()) {
                prefs.remove(CONVERSATION_SETTINGS_JSON)
            } else {
                prefs[CONVERSATION_SETTINGS_JSON] = json.encodeToString(nonEmpty)
            }
        }
    }
    suspend fun saveContextCompactEnabled(enabled: Boolean) {
        dataStore.edit { it[CONTEXT_COMPACT_ENABLED] = enabled }
    }
    suspend fun saveContextCompactModel(model: String?) {
        dataStore.edit { prefs ->
            if (model == null) prefs.remove(CONTEXT_COMPACT_MODEL) else prefs[CONTEXT_COMPACT_MODEL] = model
        }
    }
    suspend fun saveContextCompactPrompt(prompt: String) {
        dataStore.edit { prefs ->
            if (prompt.isBlank()) prefs.remove(CONTEXT_COMPACT_PROMPT) else prefs[CONTEXT_COMPACT_PROMPT] = prompt
        }
    }
    suspend fun saveContextCompactRetainCount(count: Int) {
        require(count >= 0)
        dataStore.edit { it[CONTEXT_COMPACT_RETAIN_COUNT] = count }
    }
    suspend fun saveContextCompactPreserveSystemPrompt(enabled: Boolean) {
        dataStore.edit { it[CONTEXT_COMPACT_PRESERVE_SYSTEM_PROMPT] = enabled }
    }

    suspend fun saveContextCompactThresholdPercent(percent: Int) {
        require(percent in CONTEXT_COMPACT_THRESHOLD_PERCENT_RANGE)
        dataStore.edit { it[CONTEXT_COMPACT_THRESHOLD_PERCENT] = percent }
    }

    suspend fun saveTitleGenerationModel(model: String?) {
        dataStore.edit {
            if (model == null) it.remove(TITLE_GENERATION_MODEL)
            else it[TITLE_GENERATION_MODEL] = model
        }
    }
    suspend fun saveTitleGenerationPrompt(prompt: String) {
        dataStore.edit {
            if (prompt.isBlank()) it.remove(TITLE_GENERATION_PROMPT)
            else it[TITLE_GENERATION_PROMPT] = prompt
        }
    }
    suspend fun saveImageTranscriptionEnabledModels(models: Set<String>) {
        dataStore.edit { it[IMAGE_TRANSCRIPTION_ENABLED_MODELS] = models }
    }
    suspend fun saveImageTranscriptionEnabled(enabled: Boolean) {
        dataStore.edit { it[IMAGE_TRANSCRIPTION_ENABLED] = enabled }
    }
    suspend fun saveImageTranscriptionModel(model: String?) {
        dataStore.edit {
            if (model == null) it.remove(IMAGE_TRANSCRIPTION_MODEL)
            else it[IMAGE_TRANSCRIPTION_MODEL] = model
        }
    }
    suspend fun saveImageTranscriptionBatchSize(size: Int) {
        dataStore.edit { it[IMAGE_TRANSCRIPTION_BATCH_SIZE] = size.coerceIn(1, 10) }
    }
    suspend fun saveImageTranscriptionPrompt(prompt: String) {
        dataStore.edit {
            if (prompt.isBlank()) it.remove(IMAGE_TRANSCRIPTION_PROMPT)
            else it[IMAGE_TRANSCRIPTION_PROMPT] = prompt
        }
    }
}
