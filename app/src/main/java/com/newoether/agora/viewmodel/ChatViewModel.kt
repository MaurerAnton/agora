package com.newoether.agora.viewmodel

import android.app.Application
import android.content.Context
import android.net.Uri
import com.newoether.agora.util.DebugLog
import androidx.lifecycle.AndroidViewModel
import androidx.lifecycle.ViewModel
import androidx.lifecycle.viewModelScope
import com.newoether.agora.api.*
import com.newoether.agora.data.ClaudeChatImporter
import com.newoether.agora.data.GptChatImporter
import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DataExporter
import com.newoether.agora.data.DataImporter
import com.newoether.agora.data.EmbeddingIndexer
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.EmbeddingModelType
import com.newoether.agora.data.LocalChatModelConfig
import com.newoether.agora.data.MemoryManager
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.PredefinedVariables
import com.newoether.agora.data.PromptTemplateItem
import com.newoether.agora.data.SystemPromptEntry
import com.newoether.agora.api.LlamaEngine
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.EmbeddingEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.ChatConversation
import com.newoether.agora.util.Constants
import com.newoether.agora.util.SearchResultFormatter
import com.newoether.agora.util.SnackbarEvent
import com.newoether.agora.R
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.SelectedAttachment
import com.newoether.agora.model.ToolCallData
import com.newoether.agora.service.AgoraForegroundService
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.TimeoutCancellationException
import kotlinx.coroutines.CoroutineScope
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.SupervisorJob
import kotlinx.coroutines.Job
import kotlinx.coroutines.flow.*
import kotlinx.coroutines.isActive
import kotlinx.coroutines.launch
import kotlinx.coroutines.sync.Mutex
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.coroutines.delay
import kotlinx.coroutines.withTimeout
import kotlinx.coroutines.NonCancellable
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import java.io.File
import java.util.UUID
import androidx.compose.foundation.lazy.LazyListState

private inline fun <reified T : Enum<T>> safeValueOf(name: String): T? =
    try { enumValueOf<T>(name) } catch (_: Exception) { null }

class ChatViewModel(
    application: Application,
    private val settingsManager: SettingsManager,
    private val chatDao: ChatDao,
    val memoryManager: MemoryManager,
    private val appContext: Context
) : AndroidViewModel(application) {

    private val localProvider = LocalProvider(appContext, settingsManager)

    private val builtInProviders = mapOf(
        "Google" to GeminiProvider(),
        "OpenAI" to OpenAiProvider(),
        "Anthropic" to AnthropicProvider(),
        "DeepSeek" to DeepSeekProvider(),
        "Qwen" to QwenProvider(),
        "Ollama" to OllamaProvider(),
        "Open Router" to OpenRouterProvider(),
        "Local" to localProvider
    )

    private val providers = builtInProviders.toMutableMap()

    init {
        // Auto-check for updates on launch (at most once per day)
        viewModelScope.launch(Dispatchers.IO) {
            if (settingsManager.autoUpdateCheck.first()) {
                val lastCheck = settingsManager.lastUpdateCheckTime.first()
                val now = System.currentTimeMillis()
                if (now - lastCheck > 24 * 60 * 60 * 1000L) {
                    settingsManager.saveLastUpdateCheckTime(now)
                    val info = com.newoether.agora.util.UpdateChecker.check(getCurrentVersion())
                    if (info != null) {
                        _updateDialogData.value = info
                    }
                }
            }
        }
        viewModelScope.launch(Dispatchers.IO) {
            val models = settingsManager.embeddingModels.first()
            val activeId = settingsManager.activeEmbeddingModelId.first()
            val active = models.find { it.id == activeId } ?: return@launch
            val total = chatDao.getAllMessagesForIndexing().count { it.text.isNotBlank() }
            val cached = chatDao.getEmbeddingCountByModel(active.id)
            val notCached = (total - cached).coerceAtLeast(0)
            if (notCached > 0 && !_cachingProgress.value.containsKey(active.id)) {
                _snackbarMessage.emit(SnackbarEvent(
                    "$notCached of $total messages not cached.",
                    "Cache Now"
                ) { cacheMessagesForModel(active.id) })
            }
        }
        // Clean up orphaned embeddings (messages that no longer exist)
        viewModelScope.launch(Dispatchers.IO) {
            chatDao.deleteOrphanedEmbeddings()
        }
        // Sync local chat models into available models
        viewModelScope.launch {
            settingsManager.localChatModels.collect { models ->
                val localIds = models.map { "Local:${it.modelId}" }
                settingsManager.saveAvailableModels("Local", localIds)
                // Keep aliases in sync
                val aliases = modelAliases.value.toMutableMap()
                models.forEach { aliases["Local:${it.modelId}"] = it.alias }
                settingsManager.saveModelAliases(aliases)
            }
        }
        // Sync custom providers into the providers map
        viewModelScope.launch {
            settingsManager.customProviders.collect { custom ->
                providers.keys.filter { it !in builtInProviders }.forEach { providers.remove(it) }
                val baseUrls = settingsManager.providerBaseUrls.first()
                custom.forEach { config ->
                    providers[config.name] = CustomOpenAiProvider(config.name, baseUrls[config.name] ?: "")
                }
            }
        }
        // Apply Tor SOCKS proxy settings
        viewModelScope.launch(Dispatchers.IO) {
            combine(
                settingsManager.torEnabled,
                settingsManager.torSocksPort
            ) { enabled, port -> Pair(enabled, port) }
            .collect { (enabled, port) ->
                if (enabled) {
                    com.newoether.agora.api.HttpClient.setSocksProxy("127.0.0.1", port)
                } else {
                    com.newoether.agora.api.HttpClient.disableProxy()
                }
            }
        }
        // Auto-clear available models when a provider loses its credentials.
        // Watches all three configuration sources (API keys, active key IDs,
        // and base URLs) and uses the shared isProviderConfigured() to decide
        // whether a provider just became unconfigured.
        viewModelScope.launch {
            var prevConfigured = emptyMap<String, Boolean>()

            combine(
                settingsManager.apiKeys,
                settingsManager.activeApiKeyIds,
                settingsManager.providerBaseUrls
            ) { keys, activeIds, baseUrls ->
                // Read current values; the parameters are here so combine
                // re-emits when any of the three sources changes.
                Triple(keys, activeIds, baseUrls)
            }.collect { (keys, activeIds, _) ->
                // Skip the initial empty-state emission.
                if (keys.isEmpty() && activeIds.isEmpty()) return@collect

                val current = mutableMapOf<String, Boolean>()
                providers.toMap().forEach { (name, _) ->
                    val activeKey = keys.find { it.id == activeIds[name] }?.key ?: ""
                    current[name] = isProviderConfigured(name, activeKey)
                }

                var changed = false
                current.forEach { (name, configured) ->
                    if (prevConfigured[name] == true && !configured) {
                        val existing = settingsManager.availableModels.first()[name]
                        if (!existing.isNullOrEmpty()) {
                            settingsManager.saveAvailableModels(name, emptyList())
                            changed = true
                        }
                    }
                }
                prevConfigured = current

                if (changed) {
                    val allAvailable = settingsManager.availableModels.first().values.flatten().toSet()
                    val newEnabled = enabledModels.value.intersect(allAvailable)
                    if (newEnabled != enabledModels.value) {
                        setEnabledModels(newEnabled)
                    }
                }
            }
        }
    }

    private val generationScope = CoroutineScope(SupervisorJob() + Dispatchers.IO)
    private var generationJob: Job? = null
    private val sendGate = java.util.concurrent.atomic.AtomicBoolean(false)

    private val generationManager by lazy {
        GenerationManager(
            app = application,
            chatDao = chatDao,
            memoryManager = memoryManager,
            providers = providers,
            context = appContext
        ).also { gm ->
            gm.onMessagePersisted = { messageId, text ->
                if (autoCacheEnabled.value && (modelSearchMethod.value == "rag" || manualSearchMethod.value == "rag")) {
                    indexMessageForRag(messageId, text)
                }
            }
        }
    }

    override fun onCleared() {
        super.onCleared()
        localProvider.close()
        generationScope.coroutineContext[Job]?.cancel()
    }

    val listState = LazyListState()
    val messageHeights = androidx.compose.runtime.mutableStateMapOf<String, Int>()


    fun getProviderInstance(name: String): LlmProvider {
        return providers[name] ?: GeminiProvider()
    }

    private fun getEffectiveBaseUrl(providerName: String): String? {
        return providerBaseUrls.value[providerName]
            ?: if (providerName !in builtInProviders) getProviderInstance(providerName).defaultBaseUrl
            else null
    }

    private fun isProviderConfigured(providerName: String, activeKey: String): Boolean {
        val isCustom = providerName !in builtInProviders
        return when {
            providerName == "Unknown" -> false
            providerName == "Local" -> true
            isCustom || providerName == "Ollama" -> !getEffectiveBaseUrl(providerName).isNullOrBlank()
            else -> activeKey.isNotBlank()
        }
    }

    val customProviders = settingsManager.customProviders.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())


    private val _scrollToMessage = MutableSharedFlow<String?>(replay = 0)
    val scrollToMessage = _scrollToMessage.asSharedFlow()

    fun triggerScrollToMessage(messageId: String? = null) {
        viewModelScope.launch {
            _scrollToMessage.emit(messageId)
        }
    }

    val selectedModel = settingsManager.selectedModel.stateIn(viewModelScope, SharingStarted.Eagerly, "gemini-1.5-flash")
    private val _currentActiveModel = MutableStateFlow<String?>(null)
    val currentActiveModel = kotlinx.coroutines.flow.combine(_currentActiveModel, selectedModel) { active, default ->
        active ?: default
    }.stateIn(viewModelScope, SharingStarted.Eagerly, "gemini-1.5-flash")

    fun getProviderForModel(modelId: String): String {
        // If the modelId already has a prefix (e.g., "OpenAI:gpt-4"), extract it.
        if (modelId.contains(":")) {
            return modelId.substringBefore(":")
        }
        
        // Fallback for existing or unprefixed models
        availableModels.value.forEach { (providerName, models) ->
            if (models.contains(modelId)) return providerName
        }
        
        return when {
            modelId.startsWith("gpt-") || modelId.startsWith("o1") || modelId.startsWith("o3") -> "OpenAI"
            modelId.startsWith("claude-") -> "Anthropic"
            modelId.contains("deepseek") -> "DeepSeek"
            modelId.contains("qwen") -> "Qwen"
            modelId.contains("models/") || modelId.startsWith("gemini") -> "Google"
            else -> "Unknown"
        }
    }
    
    val availableModels = settingsManager.availableModels.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())
    val enabledModels = settingsManager.enabledModels.stateIn(viewModelScope, SharingStarted.Eagerly, emptySet())
    val modelAliases = settingsManager.modelAliases.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())

    val apiKeys = settingsManager.apiKeys.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    val activeApiKeyIds = settingsManager.activeApiKeyIds.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())
    val systemPrompts = settingsManager.systemPrompts.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    val activeSystemPromptId = settingsManager.activeSystemPromptId.stateIn(viewModelScope, SharingStarted.Eagerly, null)
        
        val maxContextWindow = settingsManager.maxContextWindow.stateIn(viewModelScope, SharingStarted.Eagerly, 20)
        val visualizeContextRollout = settingsManager.visualizeContextRollout.stateIn(viewModelScope, SharingStarted.Eagerly, false)
        val codeExecutionEnabled = settingsManager.codeExecutionEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
        val googleSearchEnabled = settingsManager.googleSearchEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
        val thinkingEnabled = settingsManager.thinkingEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, true)
        val thinkingLevel = settingsManager.thinkingLevel.stateIn(viewModelScope, SharingStarted.Eagerly, "medium")
        val providerBaseUrls = settingsManager.providerBaseUrls.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())
    val titleGenerationEnabled = settingsManager.titleGenerationEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val titleGenerationModel = settingsManager.titleGenerationModel.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val imageTranscriptionEnabledModels = settingsManager.imageTranscriptionEnabledModels.stateIn(viewModelScope, SharingStarted.Eagerly, emptySet())
    val imageTranscriptionModel = settingsManager.imageTranscriptionModel.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val imageTranscriptionBatchSize = settingsManager.imageTranscriptionBatchSize.stateIn(viewModelScope, SharingStarted.Eagerly, 3)
    val accessPastConversations = settingsManager.accessPastConversations.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val accessSavedMemories = settingsManager.accessSavedMemories.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val accessActiveMemory = settingsManager.accessActiveMemory.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val ragSearchEnabled = settingsManager.ragSearchEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
    val autoCacheEnabled = settingsManager.autoCacheEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val autoUpdateCheck = settingsManager.autoUpdateCheck.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val modelSearchMethod = settingsManager.modelSearchMethod.stateIn(viewModelScope, SharingStarted.Eagerly, "keyword")
    val manualSearchMethod = settingsManager.manualSearchMethod.stateIn(viewModelScope, SharingStarted.Eagerly, "keyword")
    val embeddingModels = settingsManager.embeddingModels.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    val activeEmbeddingModelId = settingsManager.activeEmbeddingModelId.stateIn(viewModelScope, SharingStarted.Eagerly, "")
    val activeEmbeddingModel = combine(embeddingModels, activeEmbeddingModelId) { models, id ->
        models.find { it.id == id }
    }.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val localChatModels = settingsManager.localChatModels.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    private val _cachingProgress = MutableStateFlow<Map<String, Pair<Int, Int>>>(emptyMap())
    val cachingProgress: StateFlow<Map<String, Pair<Int, Int>>> = _cachingProgress.asStateFlow()
    private val cacheMutexes = java.util.concurrent.ConcurrentHashMap<String, Mutex>()
    private val _cacheCounts = MutableStateFlow<Map<String, Pair<Int, Int>>>(emptyMap())
    val cacheCounts: StateFlow<Map<String, Pair<Int, Int>>> = _cacheCounts.asStateFlow()
    fun loadCacheCounts() {
        viewModelScope.launch(Dispatchers.IO) { refreshCacheCounts() }
    }
    private suspend fun refreshCacheCounts() {
        val total = chatDao.getIndexableMessageCount()
        val counts = embeddingModels.value.associate { model ->
            val cached = chatDao.getEmbeddingCountByModel(model.id).coerceAtMost(total)
            model.id to (cached to total)
        }
        _cacheCounts.value = counts
    }
    val appLanguage = settingsManager.appLanguage.stateIn(viewModelScope, SharingStarted.Eagerly, "system")
    val webSearchEnabled = settingsManager.webSearchEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
    val webSearchProvider = settingsManager.webSearchProvider.stateIn(viewModelScope, SharingStarted.Eagerly, "brave")
    val webSearchApiKeys = settingsManager.webSearchApiKeys.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())
    val webSearchNumResults = settingsManager.webSearchNumResults.stateIn(viewModelScope, SharingStarted.Eagerly, 5)
    val webSearchBaseUrl = settingsManager.webSearchBaseUrl.stateIn(viewModelScope, SharingStarted.Eagerly, "")
    val showDocumentationFab = settingsManager.showDocumentationFab.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val shellEnabled = settingsManager.shellEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
    val shellDevices = settingsManager.shellDevices.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    val torEnabled = settingsManager.torEnabled.stateIn(viewModelScope, SharingStarted.Eagerly, false)
    val torSocksPort = settingsManager.torSocksPort.stateIn(viewModelScope, SharingStarted.Eagerly, 9050)
    val defaultTemperature = settingsManager.defaultTemperature.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val defaultMaxTokens = settingsManager.defaultMaxTokens.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val defaultTopP = settingsManager.defaultTopP.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val defaultFrequencyPenalty = settingsManager.defaultFrequencyPenalty.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val defaultPresencePenalty = settingsManager.defaultPresencePenalty.stateIn(viewModelScope, SharingStarted.Eagerly, null)
    val conversationSettings = settingsManager.conversationSettings.stateIn(viewModelScope, SharingStarted.Eagerly, emptyMap())
    val themeMode = settingsManager.themeMode.stateIn(viewModelScope, SharingStarted.Eagerly, "FOLLOW_DEVICE")
    val colorScheme = settingsManager.colorScheme.stateIn(viewModelScope, SharingStarted.Eagerly, "DEFAULT")
    val dynamicColor = settingsManager.dynamicColor.stateIn(viewModelScope, SharingStarted.Eagerly, true)
    val schemeStyle = settingsManager.schemeStyle.stateIn(viewModelScope, SharingStarted.Eagerly, "TONAL_SPOT")
    val searchContextWindow = settingsManager.searchContextWindow.stateIn(viewModelScope, SharingStarted.Eagerly, 8)
    val searchMatchLimit = settingsManager.searchMatchLimit.stateIn(viewModelScope, SharingStarted.Eagerly, 10)
    val ragThreshold = settingsManager.ragThreshold.stateIn(viewModelScope, SharingStarted.Eagerly, 0.5f)

        val conversations: StateFlow<List<ChatConversation>> = chatDao.getAllConversations()
            .map { entities ->
                entities.map { ChatConversation(id = it.id, title = it.title, systemPromptId = it.systemPromptId, modelId = it.modelId) }
            }.stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())
    private val _currentConversationId = MutableStateFlow<String?>(null)
    val currentConversationId: StateFlow<String?> = _currentConversationId.asStateFlow()

    private val _allMessages = MutableStateFlow<List<ChatMessage>>(emptyList())
    val allMessages: StateFlow<List<ChatMessage>> = _allMessages.asStateFlow()

    private val _isSyncingModels = MutableStateFlow(false)
    val isSyncingModels: StateFlow<Boolean> = _isSyncingModels.asStateFlow()

    private val _snackbarMessage = MutableSharedFlow<SnackbarEvent>(replay = 1)
    val snackbarMessage = _snackbarMessage.asSharedFlow()
    fun emitSnackbar(message: String, actionLabel: String? = null, onAction: (() -> Unit)? = null) {
        viewModelScope.launch { _snackbarMessage.emit(SnackbarEvent(message, actionLabel, onAction)) }
    }

    private val _updateDialogData = MutableStateFlow<com.newoether.agora.util.UpdateInfo?>(null)
    val updateDialogData: StateFlow<com.newoether.agora.util.UpdateInfo?> = _updateDialogData.asStateFlow()
    fun dismissUpdateDialog() { _updateDialogData.value = null }

    private val _previewPdfPages = MutableStateFlow<List<String>>(emptyList())
    val previewPdfPages: StateFlow<List<String>> = _previewPdfPages.asStateFlow()
    private val _previewPdfIndex = MutableStateFlow(0)
    val previewPdfIndex: StateFlow<Int> = _previewPdfIndex.asStateFlow()

    private val _previewFileContent = MutableStateFlow<String?>(null)
    val previewFileContent: StateFlow<String?> = _previewFileContent.asStateFlow()
    private val _previewFileName = MutableStateFlow<String?>(null)
    val previewFileName: StateFlow<String?> = _previewFileName.asStateFlow()

    fun showPdfPreview(pages: List<String>, startIndex: Int) {
        _previewPdfPages.value = pages
        _previewPdfIndex.value = startIndex
    }

    fun showFilePreview(fileName: String, content: String) {
        _previewFileName.value = fileName
        _previewFileContent.value = content
    }

    fun clearPreviews() {
        _previewPdfPages.value = emptyList()
        _previewFileContent.value = null
    }

    private val _streamingMessage = MutableStateFlow<ChatMessage?>(null)
    private val _selectedChildren = MutableStateFlow<Map<String?, String>>(emptyMap())

    val messages: StateFlow<List<ChatMessage>> = combine(
        _allMessages,
        _streamingMessage,
        _selectedChildren
    ) { allMsgs, streaming, selectedChildren ->
        val path = mutableListOf<ChatMessage>()
        var currentParentId: String? = null
        
        while (true) {
            val siblings = allMsgs.filter { it.parentId == currentParentId }
                .sortedBy { it.timestamp }
            
            if (siblings.isEmpty()) break
            
            val selectedId = selectedChildren[currentParentId]
            val visibleSiblings = siblings.filter {
                !it.id.startsWith(Constants.TOOL_MSG_PREFIX) && !it.id.startsWith(Constants.RESULT_MSG_PREFIX)
            }
            var selectedMessage = if (visibleSiblings.isNotEmpty()) {
                visibleSiblings.find { it.id == selectedId } ?: visibleSiblings.last()
            } else {
                siblings.find { it.id == selectedId } ?: siblings.last()
            }

            if (streaming != null && selectedMessage.id == streaming.id) {
                selectedMessage = streaming
            }

            // Skip synthetic tool call/result messages (hidden from UI, API context only)
            val isSynthetic = selectedMessage.id.startsWith(Constants.TOOL_MSG_PREFIX) || selectedMessage.id.startsWith(Constants.RESULT_MSG_PREFIX)
            if (!isSynthetic || (streaming != null && selectedMessage.id == streaming.id)) {
                path.add(selectedMessage)
            }
            currentParentId = selectedMessage.id
        }

        if (streaming != null && path.none { it.id == streaming.id }) {
            if (streaming.parentId == path.lastOrNull()?.id || (streaming.parentId == null && path.isEmpty())) {
                path.add(streaming)
            }
        }
        path
    }.distinctUntilChanged()
    .flowOn(Dispatchers.Default)
    .stateIn(viewModelScope, SharingStarted.Eagerly, emptyList())

    val totalTokens: StateFlow<Int> = _allMessages.map { list ->
        list.sumOf { it.tokenCount }
    }.stateIn(viewModelScope, SharingStarted.Eagerly, 0)

    private val _isLoading = MutableStateFlow(false)
    val isLoading: StateFlow<Boolean> = _isLoading.asStateFlow()
    private val _generatingInConversationId = MutableStateFlow<String?>(null)
    val generatingInConversationId: StateFlow<String?> = _generatingInConversationId.asStateFlow()

    private val _isSwitching = MutableStateFlow(false)
    val isSwitching: StateFlow<Boolean> = _isSwitching.asStateFlow()

    private var switchingJob: Job? = null

    fun setSwitching(switching: Boolean) {
        _isSwitching.value = switching
    }

    fun clearMessageHeights() {
        messageHeights.clear()
    }

    private val _isNewChatMode = MutableStateFlow(true)
    val isNewChatMode: StateFlow<Boolean> = _isNewChatMode.asStateFlow()

    private val _isTransitioningToNewChat = MutableStateFlow(false)
    val isTransitioningToNewChat: StateFlow<Boolean> = _isTransitioningToNewChat.asStateFlow()

    private val _pendingSystemPromptId = MutableStateFlow<String?>(null)
    val pendingSystemPromptId: StateFlow<String?> = _pendingSystemPromptId.asStateFlow()

    fun setPendingSystemPrompt(promptId: String?) {
        _pendingSystemPromptId.value = promptId
    }

    private val _pendingConversationSettings = MutableStateFlow<ConversationSettings?>(null)
    val pendingConversationSettings: StateFlow<ConversationSettings?> = _pendingConversationSettings.asStateFlow()

    fun setPendingConversationSettings(settings: ConversationSettings?) {
        _pendingConversationSettings.value = settings
    }

    fun updateConversationSetting(convId: String?, update: (ConversationSettings) -> ConversationSettings) {
        if (convId != null) {
            val current = conversationSettings.value[convId] ?: ConversationSettings()
            viewModelScope.launch { settingsManager.saveConversationSettings(convId, update(current)) }
        } else {
            val current = _pendingConversationSettings.value ?: ConversationSettings()
            _pendingConversationSettings.value = update(current)
        }
    }

    private val _branchSwitchTrigger = MutableStateFlow<String?>(null)
    val branchSwitchTrigger: StateFlow<String?> = _branchSwitchTrigger.asStateFlow()

    // Export/Import state
    private val _exportProgress = MutableStateFlow<Float?>(null)
    val exportProgress: StateFlow<Float?> = _exportProgress.asStateFlow()

    private val _importProgress = MutableStateFlow<Float?>(null)
    val importProgress: StateFlow<Float?> = _importProgress.asStateFlow()

    private val _importManifest = MutableStateFlow<DataImporter.ImportManifest?>(null)
    val importManifest: StateFlow<DataImporter.ImportManifest?> = _importManifest.asStateFlow()

    private val _importPreview = MutableStateFlow<DataImporter.ImportPreview?>(null)
    val importPreview: StateFlow<DataImporter.ImportPreview?> = _importPreview.asStateFlow()

    // Claude import state
    private val _claudeImportPreview = MutableStateFlow<ClaudeChatImporter.ImportPreview?>(null)
    val claudeImportPreview: StateFlow<ClaudeChatImporter.ImportPreview?> = _claudeImportPreview.asStateFlow()

    private val _claudeImportProgress = MutableStateFlow<Float?>(null)
    val claudeImportProgress: StateFlow<Float?> = _claudeImportProgress.asStateFlow()

    private val _claudeImportResult = MutableStateFlow<ClaudeChatImporter.ImportResult?>(null)
    val claudeImportResult: StateFlow<ClaudeChatImporter.ImportResult?> = _claudeImportResult.asStateFlow()

    // GPT import state
    private val _gptImportPreview = MutableStateFlow<GptChatImporter.ImportPreview?>(null)
    val gptImportPreview: StateFlow<GptChatImporter.ImportPreview?> = _gptImportPreview.asStateFlow()

    private val _gptImportProgress = MutableStateFlow<Float?>(null)
    val gptImportProgress: StateFlow<Float?> = _gptImportProgress.asStateFlow()

    private val _gptImportResult = MutableStateFlow<GptChatImporter.ImportResult?>(null)
    val gptImportResult: StateFlow<GptChatImporter.ImportResult?> = _gptImportResult.asStateFlow()


    private val _conversationCount = MutableStateFlow(0)
    val conversationCount: StateFlow<Int> = _conversationCount.asStateFlow()

    private val _memoryCount = MutableStateFlow(0)
    val memoryCount: StateFlow<Int> = _memoryCount.asStateFlow()

    private val _systemPromptCount = MutableStateFlow(0)
    val systemPromptCount: StateFlow<Int> = _systemPromptCount.asStateFlow()

    init {
        viewModelScope.launch {
            _currentConversationId.collectLatest { id ->
                if (id != null) {
                    // Fix stuck sending states when loading conversation
                    val stuckMessages = chatDao.getMessagesForConversation(id).first()
                        .filter { it.status == MessageStatus.SENDING || it.status == MessageStatus.THINKING || it.status == MessageStatus.TOOL_CALLING || it.status == MessageStatus.TRANSCRIBING }
                    
                    stuckMessages.forEach { msg ->
                        chatDao.upsertMessage(msg.copy(status = MessageStatus.STOPPED))
                    }

                    // Restore selected branches
                    val conversation = chatDao.getConversation(id)
                    if (conversation?.selectedBranchesJson != null) {
                        try {
                            val map = Json.decodeFromString<Map<String, String>>(conversation.selectedBranchesJson)
                            val decodedMap = map.mapKeys { if (it.key == "null") null else it.key }
                            _selectedChildren.value = decodedMap
                        } catch (e: Exception) {
                            _selectedChildren.value = emptyMap()
                        }
                    } else {
                        _selectedChildren.value = emptyMap()
                    }

                    chatDao.getMessagesForConversation(id).collect { entities ->
                        val mapped = entities.map {
                            ChatMessage(
                                id = it.id,
                                parentId = it.parentId,
                                text = SearchResultFormatter.format(it.text, appContext),
                                images = it.images,
                                thoughts = it.thoughts,
                                thoughtTitle = it.thoughtTitle,
                                tokenCount = it.tokenCount,
                                status = it.status,
                                participant = it.participant,
                                timestamp = it.timestamp,
                                thoughtTimeMs = it.thoughtTimeMs,
                                modelName = it.modelName,
                                segments = it.toolCallJson?.let { json ->
                                    try { Json.decodeFromString<List<MessageSegment>>(json) } catch (_: Exception) { null }
                                } ?: it.thoughts?.takeIf { t -> t.isNotBlank() }?.let { listOf(MessageSegment(type = "thought", content = it)) },
                                toolCall = it.toolCallJson?.let { json ->
                                    try {
                                        val segs = Json.decodeFromString<List<MessageSegment>>(json)
                                        segs.lastOrNull { s -> s.type == "tool" }?.let { s ->
                                            val rawResult = s.toolResult ?: ""
                                            ToolCallData(s.toolName ?: "", s.toolArgs ?: "{}", SearchResultFormatter.format(rawResult, appContext))
                                        }
                                    } catch (_: Exception) { null }
                                },
                                attachmentMeta = it.attachmentMeta?.let { json ->
                                    try { Json.decodeFromString<AttachmentMeta>(json) } catch (_: Exception) { null }
                                }
                            )
                        }
                        // Backfill toolCall for old result_ messages persisted without toolCallJson.
                        // They inherit the parent tool_ message's ToolCallData so the provider can
                        // format them as proper "tool" role messages with matching tool_call_id.
                        _allMessages.value = mapped.map { msg ->
                            if (msg.id.startsWith(Constants.RESULT_MSG_PREFIX) && msg.toolCall == null) {
                                val parentTool = mapped.find { it.id == msg.parentId }
                                if (parentTool != null && parentTool.toolCall != null) {
                                    msg.copy(toolCall = parentTool.toolCall)
                                } else msg
                            } else msg
                        }
                    }
                } else {
                    _allMessages.value = emptyList()
                    _selectedChildren.value = emptyMap()
                    messageHeights.clear()
                }
                // Prune stale height entries for messages no longer in the conversation
                if (_allMessages.value.isNotEmpty()) {
                    val currentIds = _allMessages.value.map { it.id }.toSet()
                    messageHeights.keys.retainAll { it in currentIds }
                }
            }
        }
        
        viewModelScope.launch {
            _selectedChildren.collect { childrenMap ->
                val id = _currentConversationId.value
                if (id != null) {
                    val conversation = chatDao.getConversation(id)
                    if (conversation != null) {
                        val stringKeyMap = childrenMap.mapKeys { it.key ?: "null" }
                        val json = Json.encodeToString(stringKeyMap)
                        if (conversation.selectedBranchesJson != json) {
                            chatDao.upsertConversation(conversation.copy(selectedBranchesJson = json))
                        }
                    }
                }
            }
        }
    }

    // Settings logic
        fun setSelectedModel(model: String) {
            viewModelScope.launch { settingsManager.saveSelectedModel(model) }
        }
    
    fun setEnabledModels(models: Set<String>) { 
        viewModelScope.launch { 
            settingsManager.saveEnabledModels(models) 
            if (!models.contains(selectedModel.value)) {
                settingsManager.saveSelectedModel(models.firstOrNull() ?: "")
            }
        } 
    }

    fun updateModelAlias(model: String, alias: String) {
        viewModelScope.launch {
            val currentAliases = modelAliases.value.toMutableMap()
            if (alias.isBlank()) {
                currentAliases.remove(model)
            } else {
                currentAliases[model] = alias
            }
            settingsManager.saveModelAliases(currentAliases)
        }
    }

    fun addApiKey(name: String, key: String, provider: String) {
        viewModelScope.launch {
            val entry = ApiKeyEntry(name = name, key = key, provider = provider)
            val newList = apiKeys.value + entry
            settingsManager.saveApiKeys(newList)
            settingsManager.setActiveApiKeyId(provider, entry.id)
        }
    }
    fun deleteApiKey(id: String) {
        viewModelScope.launch {
            val entry = apiKeys.value.find { it.id == id } ?: return@launch
            val provider = entry.provider
            val newList = apiKeys.value.filter { it.id != id }
            // Update active key FIRST so the auto-clear collector sees the
            // replacement key (if any) before the old key is removed from the list.
            if (activeApiKeyIds.value[provider] == id) {
                val other = newList.firstOrNull { it.provider == provider }
                settingsManager.setActiveApiKeyId(provider, other?.id)
            }
            settingsManager.saveApiKeys(newList)
        }
    }
    fun updateApiKey(id: String, name: String, key: String) {
        viewModelScope.launch {
            val newList = apiKeys.value.map { if (it.id == id) it.copy(name = name, key = key) else it }
            settingsManager.saveApiKeys(newList)
        }
    }
    fun setActiveApiKey(provider: String, id: String) { viewModelScope.launch { settingsManager.setActiveApiKeyId(provider, id) } }

    fun addSystemPrompt(
        title: String,
        systemItems: List<PromptTemplateItem>,
        userPrependItems: List<PromptTemplateItem>,
        userPostpendItems: List<PromptTemplateItem>
    ) {
        viewModelScope.launch {
            val newList = systemPrompts.value + SystemPromptEntry(
                title = title,
                systemItems = systemItems,
                userPrependItems = userPrependItems,
                userPostpendItems = userPostpendItems
            )
            settingsManager.saveSystemPrompts(newList)
            if (activeSystemPromptId.value == null) settingsManager.setActiveSystemPromptId(newList.last().id)
        }
    }
    fun deleteSystemPrompt(id: String) {
        viewModelScope.launch {
            val newList = systemPrompts.value.filter { it.id != id }
            settingsManager.saveSystemPrompts(newList)
            if (activeSystemPromptId.value == id) settingsManager.setActiveSystemPromptId(newList.firstOrNull()?.id)
        }
    }
    fun updateSystemPrompt(
        id: String,
        title: String,
        systemItems: List<PromptTemplateItem>,
        userPrependItems: List<PromptTemplateItem>,
        userPostpendItems: List<PromptTemplateItem>
    ) {
        viewModelScope.launch {
            val newList = systemPrompts.value.map {
                if (it.id == id) it.copy(
                    title = title,
                    systemItems = systemItems,
                    userPrependItems = userPrependItems,
                    userPostpendItems = userPostpendItems
                ) else it
            }
            settingsManager.saveSystemPrompts(newList)
        }
    }
    fun setActiveSystemPrompt(id: String) { viewModelScope.launch { settingsManager.setActiveSystemPromptId(id) } }
    fun setMaxContextWindow(window: Int) { viewModelScope.launch { settingsManager.saveMaxContextWindow(window) } }
    fun setVisualizeContextRollout(enabled: Boolean) { viewModelScope.launch { settingsManager.saveVisualizeContextRollout(enabled) } }
    fun setProviderBaseUrl(provider: String, url: String) { viewModelScope.launch { settingsManager.saveProviderBaseUrl(provider, url) } }
    fun addCustomProvider(name: String, baseUrl: String) {
        providers[name] = CustomOpenAiProvider(name, baseUrl)
        viewModelScope.launch {
            settingsManager.saveProviderBaseUrl(name, baseUrl)
            val current = customProviders.value.toMutableList()
            current.add(CustomProviderConfig(name))
            settingsManager.saveCustomProviders(current)
        }
    }
    fun renameCustomProvider(oldName: String, newName: String) {
        val url = providerBaseUrls.value[oldName] ?: return
        providers.remove(oldName)
        providers[newName] = CustomOpenAiProvider(newName, url)
        viewModelScope.launch {
            val current = customProviders.value.toMutableList()
            val idx = current.indexOfFirst { it.name == oldName }
            if (idx >= 0) {
                current[idx] = CustomProviderConfig(newName)
                settingsManager.saveCustomProviders(current)
                settingsManager.saveProviderBaseUrl(oldName, "")
                settingsManager.saveProviderBaseUrl(newName, url)
                val models = availableModels.value.toMutableMap()
                models[newName] = models.remove(oldName) ?: emptyList()
                settingsManager.saveAvailableModels(newName, models[newName] ?: emptyList())
                settingsManager.saveAvailableModels(oldName, emptyList())
                val enabled = enabledModels.value.map { if (it.startsWith("$oldName:")) it.replace("$oldName:", "$newName:") else it }.toSet()
                setEnabledModels(enabled)
                val aliases = modelAliases.value.mapKeys { if (it.key.startsWith("$oldName:")) it.key.replace("$oldName:", "$newName:") else it.key }
                settingsManager.saveModelAliases(aliases)
                settingsManager.setActiveApiKeyId(oldName, null)
                val keys = apiKeys.value.map { if (it.provider == oldName) it.copy(provider = newName) else it }
                settingsManager.saveApiKeys(keys)
                val activeKeyIds = activeApiKeyIds.value.toMutableMap()
                activeKeyIds[oldName]?.let { activeKeyIds[newName] = it; activeKeyIds.remove(oldName) }
                activeKeyIds.forEach { (provider, id) -> settingsManager.setActiveApiKeyId(provider, id) }
            }
        }
    }
    fun deleteCustomProvider(name: String) {
        viewModelScope.launch {
            val current = customProviders.value.toMutableList()
            current.removeAll { it.name == name }
            settingsManager.saveCustomProviders(current)
            providers.remove(name)
            // Clean up associated data
            settingsManager.saveAvailableModels(name, emptyList())
            val enabled = enabledModels.value.filter { !it.startsWith("$name:") }.toSet()
            setEnabledModels(enabled)
            val aliases = modelAliases.value.filterKeys { !it.startsWith("$name:") }
            settingsManager.saveModelAliases(aliases)
            val baseUrls = providerBaseUrls.value.toMutableMap()
            baseUrls.remove(name)
            settingsManager.saveProviderBaseUrl(name, "")
            val keys = apiKeys.value.filter { it.provider != name }
            settingsManager.saveApiKeys(keys)
            settingsManager.setActiveApiKeyId(name, null)
        }
    }
    fun setTitleGenerationEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveTitleGenerationEnabled(enabled) } }
    fun setTitleGenerationModel(model: String?) { viewModelScope.launch { settingsManager.saveTitleGenerationModel(model) } }
    fun setImageTranscriptionModel(model: String?) { viewModelScope.launch { settingsManager.saveImageTranscriptionModel(model) } }
    fun setImageTranscriptionBatchSize(size: Int) { viewModelScope.launch { settingsManager.saveImageTranscriptionBatchSize(size) } }
    fun addImageTranscriptionModels(models: Set<String>) { viewModelScope.launch { settingsManager.saveImageTranscriptionEnabledModels(imageTranscriptionEnabledModels.value + models) } }
    fun removeImageTranscriptionModel(model: String) { viewModelScope.launch { settingsManager.saveImageTranscriptionEnabledModels(imageTranscriptionEnabledModels.value - model) } }

    private fun resolveTranscriptionProviderName(): String =
        imageTranscriptionModel.value?.let { getProviderForModel(it) } ?: ""

    private fun resolveTranscriptionModelId(): String =
        imageTranscriptionModel.value?.substringAfter(":") ?: ""

    private fun resolveTranscriptionApiKey(): String {
        val model = imageTranscriptionModel.value ?: return ""
        val providerName = getProviderForModel(model)
        if (providerName == "Local") return ""
        val activeKeyId = activeApiKeyIds.value[providerName]
        return apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
    }

    private fun resolveTranscriptionBaseUrl(): String? {
        val model = imageTranscriptionModel.value ?: return null
        val providerName = getProviderForModel(model)
        return providerBaseUrls.value[providerName]
            ?: if (providerName !in builtInProviders) getProviderInstance(providerName).defaultBaseUrl
            else null
    }

    fun setAccessPastConversations(enabled: Boolean) { viewModelScope.launch { settingsManager.saveAccessPastConversations(enabled) } }
    fun setAccessSavedMemories(enabled: Boolean) { viewModelScope.launch { settingsManager.saveAccessSavedMemories(enabled) } }
    fun setAccessActiveMemory(enabled: Boolean) { viewModelScope.launch { settingsManager.saveAccessActiveMemory(enabled) } }
    fun setRagSearchEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveRagSearchEnabled(enabled) } }
    fun setAutoCacheEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveAutoCacheEnabled(enabled) } }
    fun setAutoUpdateCheck(enabled: Boolean) { viewModelScope.launch { settingsManager.saveAutoUpdateCheck(enabled) } }
    fun getCurrentVersion(): String {
        return try { appContext.packageManager.getPackageInfo(appContext.packageName, 0).versionName ?: "?" } catch (_: Exception) { "?" }
    }
    suspend fun checkForUpdates(): com.newoether.agora.util.UpdateInfo? {
        val current = getCurrentVersion()
        return com.newoether.agora.util.UpdateChecker.check(current)
    }
    fun setModelSearchMethod(method: String) { viewModelScope.launch { settingsManager.saveModelSearchMethod(method) } }
    fun setManualSearchMethod(method: String) { viewModelScope.launch { settingsManager.saveManualSearchMethod(method) } }
    fun addEmbeddingModel(config: EmbeddingModelConfig) {
        viewModelScope.launch {
            val wasEmpty = embeddingModels.value.isEmpty()
            val models = embeddingModels.value.toMutableList()
            models.add(config)
            settingsManager.saveEmbeddingModels(models)
            if (wasEmpty) {
                settingsManager.setActiveEmbeddingModelId(config.id)
            }
            refreshCacheCounts()
        }
    }
    fun deleteEmbeddingModel(id: String) {
        viewModelScope.launch(Dispatchers.IO) {
            val mutex = cacheMutexes.computeIfAbsent(id) { Mutex() }
            mutex.withLock {
                val model = embeddingModels.value.find { it.id == id }
                if (model?.type == EmbeddingModelType.LOCAL && model.localFilePath.isNotBlank()) {
                    java.io.File(model.localFilePath).delete()
                }
                chatDao.deleteEmbeddingsByModel(id)
                val models = embeddingModels.value.filter { it.id != id }
                settingsManager.saveEmbeddingModels(models)
                if (activeEmbeddingModelId.value == id && models.isNotEmpty()) {
                    settingsManager.setActiveEmbeddingModelId(models.first().id)
                }
                refreshCacheCounts()
            }
        }
    }
    fun renameEmbeddingModel(id: String, newName: String, batchSize: Int? = null) {
        viewModelScope.launch {
            val models = embeddingModels.value.map {
                if (it.id == id) it.copy(name = newName, batchSize = batchSize ?: it.batchSize) else it
            }
            settingsManager.saveEmbeddingModels(models)
        }
    }
    fun setActiveEmbeddingModel(id: String) {
        if (id == activeEmbeddingModelId.value) return
        viewModelScope.launch(Dispatchers.IO) {
            settingsManager.setActiveEmbeddingModelId(id)
            val model = embeddingModels.value.find { it.id == id } ?: return@launch
            val total = chatDao.getAllMessagesForIndexing().count { it.text.isNotBlank() }
            val cached = chatDao.getEmbeddingCountByModel(id)
            val notCached = (total - cached).coerceAtLeast(0)
            if (notCached > 0) {
                if (cachingProgress.value.containsKey(id)) {
                    _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.embedding_model_caching, model.name)))
                } else {
                    _snackbarMessage.emit(SnackbarEvent(
                        appContext.getString(R.string.messages_not_cached, notCached, total),
                        appContext.getString(R.string.cache_now)
                    ) { cacheMessagesForModel(id) })
                }
            }
        }
    }
    fun cacheMessagesForModel(modelId: String, recache: Boolean = false, silent: Boolean = false) {
        viewModelScope.launch(Dispatchers.IO) {
            val mutex = cacheMutexes.computeIfAbsent(modelId) { Mutex() }
            mutex.withLock {
                val model = embeddingModels.value.find { it.id == modelId } ?: return@launch
                if (recache) {
                    chatDao.deleteEmbeddingsByModel(modelId)
                }
                val allMessages = chatDao.getAllMessagesForIndexing().filter { it.text.isNotBlank() }
                val total = allMessages.size
                if (total == 0) {
                    if (!silent) _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.no_messages_to_cache)))
                    refreshCacheCounts()
                    return@launch
                }
                val existingIds = chatDao.getEmbeddedMessageIdsByModel(modelId).toSet()
                val toProcess = allMessages.filter { it.id !in existingIds }
                if (toProcess.isEmpty()) {
                    if (!silent) _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.all_messages_already_cached, total)))
                    refreshCacheCounts()
                    return@launch
                }
                val alreadyDone = total - toProcess.size
                var succeeded = 0
                var attempted = 0
                _cachingProgress.update { it + (modelId to (alreadyDone to total)) }
                try {
                    val batchSize = model.batchSize.coerceIn(1, 100)
                    if (model.type == EmbeddingModelType.LOCAL) {
                        if (!LlamaEngine.isModelReady(model.localFilePath)) {
                            if (!silent) _snackbarMessage.emit(SnackbarEvent("Local model file not found. Please re-import the model."))
                            return@launch
                        }
                        toProcess.chunked(batchSize).forEach { batch ->
                            if (embeddingModels.value.none { it.id == modelId }) return@launch
                            val texts = batch.map { it.text.take(Constants.MAX_EMBEDDING_TEXT_LENGTH) }
                            val embeddings = LlamaEngine.computeEmbeddings(texts, model.localFilePath) {
                                localProvider.releaseEngineBlocking()
                            }
                            batch.zip(embeddings).forEach { (msg, embd) ->
                                attempted++
                                if (embd != null) {
                                    chatDao.upsertEmbedding(EmbeddingEntity(
                                        messageId = msg.id, modelId = modelId,
                                        embedding = EmbeddingIndexer.floatsToBytes(embd),
                                        chunkText = msg.text.take(Constants.MAX_CHUNK_TEXT_LENGTH), dimension = embd.size
                                    ))
                                    succeeded++
                                }
                            }
                            _cachingProgress.update { it + (modelId to (alreadyDone + attempted to total)) }
                        }
                    } else {
                        val apiKey = model.remoteApiKey.ifBlank { resolveEmbeddingApiKey() ?: "" }
                        if (apiKey.isBlank()) {
                            if (!silent) _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.no_api_key_configured)))
                            return@launch
                        }
                        val baseUrl = model.remoteBaseUrl.ifBlank { resolveEmbeddingBaseUrl() }
                        toProcess.chunked(batchSize).forEach { batch ->
                            if (embeddingModels.value.none { it.id == modelId }) return@launch
                            val texts = batch.map { it.text.take(Constants.MAX_EMBEDDING_TEXT_LENGTH) }
                            val embeddings = EmbeddingClient.computeEmbeddings(
                                texts, apiKey, model.remoteModelName, baseUrl
                            )
                            batch.zip(embeddings).forEach { (msg, embd) ->
                                attempted++
                                if (embd != null) {
                                    chatDao.upsertEmbedding(EmbeddingEntity(
                                        messageId = msg.id, modelId = modelId,
                                        embedding = EmbeddingIndexer.floatsToBytes(embd),
                                        chunkText = msg.text.take(Constants.MAX_CHUNK_TEXT_LENGTH), dimension = embd.size
                                    ))
                                    succeeded++
                                }
                            }
                            _cachingProgress.update { it + (modelId to (alreadyDone + attempted to total)) }
                        }
                    }
                } finally {
                    _cachingProgress.update { it - modelId }
                }
                val failed = toProcess.size - succeeded
                if (!silent) {
                    if (failed == 0) {
                        _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.all_messages_cached, total)))
                    } else {
                        _snackbarMessage.emit(SnackbarEvent(
                            appContext.getString(R.string.cached_partial_failed, succeeded, toProcess.size, failed),
                            appContext.getString(R.string.retry)
                        ) { cacheMessagesForModel(modelId) })
                    }
                }
                chatDao.deleteOrphanedEmbeddings()
                refreshCacheCounts()
            }
        }
    }

    fun isLocalModelIdTaken(modelId: String, excludeId: String? = null): Boolean {
        return localChatModels.value.any { it.modelId == modelId && it.id != excludeId }
    }

    fun addLocalChatModel(config: LocalChatModelConfig) {
        viewModelScope.launch {
            if (isLocalModelIdTaken(config.modelId)) return@launch
            val models = localChatModels.value.toMutableList()
            models.add(config)
            settingsManager.saveLocalChatModels(models)
            val modelPrefixedId = "Local:${config.modelId}"
            setEnabledModels(enabledModels.value + modelPrefixedId)
            settingsManager.saveModelAliases(modelAliases.value + (modelPrefixedId to config.alias))
        }
    }
    fun deleteLocalChatModel(uuid: String) {
        viewModelScope.launch(Dispatchers.IO) {
            val model = localChatModels.value.find { it.id == uuid }
            if (model != null) {
                if (model.localFilePath.isNotBlank()) java.io.File(model.localFilePath).delete()
                if (model.mmprojPath.isNotBlank()) java.io.File(model.mmprojPath).delete()
            }
            val models = localChatModels.value.filter { it.id != uuid }
            settingsManager.saveLocalChatModels(models)
            val modelPrefixedId = "Local:${model?.modelId ?: uuid}"
            setEnabledModels(enabledModels.value - modelPrefixedId)
            val updatedAvailable = settingsManager.availableModels.first().toMutableMap()
            updatedAvailable["Local"] = models.map { "Local:${it.modelId}" }
            settingsManager.saveAvailableModels("Local", updatedAvailable["Local"] ?: emptyList())
            settingsManager.saveModelAliases(modelAliases.value - modelPrefixedId)
        }
    }
    fun updateLocalChatModel(
        uuid: String, newModelId: String, newAlias: String, nCtx: Int, temperature: Float, topP: Float, maxTokens: Int,
        mmprojPath: String = ""
    ) {
        viewModelScope.launch {
            if (isLocalModelIdTaken(newModelId, excludeId = uuid)) return@launch
            val oldModel = localChatModels.value.find { it.id == uuid } ?: return@launch
            if (oldModel.mmprojPath.isNotBlank() && oldModel.mmprojPath != mmprojPath) {
                java.io.File(oldModel.mmprojPath).delete()
            }
            val models = localChatModels.value.map {
                if (it.id == uuid) it.copy(modelId = newModelId, alias = newAlias, nCtx = nCtx, temperature = temperature, topP = topP, maxTokens = maxTokens, mmprojPath = mmprojPath)
                else it
            }
            settingsManager.saveLocalChatModels(models)
            // Update model references if modelId changed
            if (oldModel.modelId != newModelId) {
                val oldPrefixed = "Local:${oldModel.modelId}"
                val newPrefixed = "Local:$newModelId"
                setEnabledModels(enabledModels.value - oldPrefixed + newPrefixed)
                val avail = settingsManager.availableModels.first().toMutableMap()
                avail["Local"] = models.map { "Local:${it.modelId}" }
                settingsManager.saveAvailableModels("Local", avail["Local"] ?: emptyList())
                settingsManager.saveModelAliases(modelAliases.value - oldPrefixed + (newPrefixed to newAlias))
            } else {
                settingsManager.saveModelAliases(modelAliases.value + ("Local:$newModelId" to newAlias))
            }
        }
    }

    suspend fun semanticSearch(query: String, limit: Int = 20): List<Pair<MessageEntity, Float>> {
        val ctx = com.newoether.agora.viewmodel.GenerationContext(
            accessSavedMemories = accessSavedMemories.value,
            accessActiveMemory = accessActiveMemory.value,
            accessPastConversations = accessPastConversations.value,
            modelSearchMethod = modelSearchMethod.value,
            activeEmbeddingConfig = activeEmbeddingModel.value,
            embeddingApiKey = resolveEmbeddingApiKey() ?: "",
            ragThreshold = ragThreshold.value,
            searchMatchLimit = searchMatchLimit.value,
            searchContextWindow = searchContextWindow.value,
            webSearchEnabled = webSearchEnabled.value,
            webSearchApiKeys = webSearchApiKeys.value,
            webSearchProvider = webSearchProvider.value,
            webSearchNumResults = webSearchNumResults.value,
            webSearchBaseUrl = webSearchBaseUrl.value
        )
        return generationManager.semanticSearch(query, limit, ctx)
    }

    private suspend fun resolveEmbedding(text: String): FloatArray? = withContext(Dispatchers.IO) {
        val model = activeEmbeddingModel.value
        if (model == null) {
            DebugLog.w("AgoraVM", "resolveEmbedding: no active model")
            return@withContext null
        }
        if (model.type == EmbeddingModelType.LOCAL) {
            if (!LlamaEngine.isModelReady(model.localFilePath)) {
                DebugLog.w("AgoraVM", "resolveEmbedding: local model not ready at ${model.localFilePath}")
                return@withContext null
            }
            DebugLog.d("AgoraVM", "resolveEmbedding: using local model ${model.name}")
            LlamaEngine.computeEmbedding(text, model.localFilePath) {
                localProvider.releaseEngineBlocking()
            }
        } else {
            val apiKey = model.remoteApiKey.ifBlank { resolveEmbeddingApiKey() ?: "" }
            if (apiKey.isBlank()) {
                DebugLog.w("AgoraVM", "resolveEmbedding: no API key available")
                return@withContext null
            }
            val baseUrl = model.remoteBaseUrl.ifBlank { resolveEmbeddingBaseUrl() }
            DebugLog.d("AgoraVM", "resolveEmbedding: calling ${model.remoteModelName} @ $baseUrl")
            EmbeddingClient.computeEmbedding(text, apiKey, model.remoteModelName, baseUrl)
        }
    }

    private fun resolveEmbeddingApiKey(): String? {
        val keys = apiKeys.value
        for (entry in keys) {
            if (entry.provider == "OpenAI" || entry.provider == "DeepSeek" || entry.provider == "Qwen" || entry.provider == "Open Router") {
                return entry.key
            }
        }
        return keys.firstOrNull()?.key
    }

    data class EmbeddingKeyInfo(val provider: String, val key: String, val baseUrl: String)

    /** Exact match only — for UI display in the embedding dialog. No fallback. */
    fun resolveEmbeddingKeyForProviderExact(targetProvider: String): EmbeddingKeyInfo? {
        val keys = apiKeys.value
        val match = keys.find { it.provider.equals(targetProvider, ignoreCase = true) }
        if (match != null) {
            val baseUrl = providerBaseUrls.value[match.provider] ?: resolveDefaultBaseUrlForProvider(match.provider)
            return EmbeddingKeyInfo(match.provider, match.key, baseUrl)
        }
        return null
    }

    /** With fallback — for runtime API calls. */
    private fun resolveEmbeddingKeyForProvider(targetProvider: String): EmbeddingKeyInfo? {
        val keys = apiKeys.value
        val match = keys.find { it.provider.equals(targetProvider, ignoreCase = true) }
        if (match != null) {
            val baseUrl = providerBaseUrls.value[match.provider] ?: resolveDefaultBaseUrlForProvider(match.provider)
            return EmbeddingKeyInfo(match.provider, match.key, baseUrl)
        }
        val fallbackKeys = listOf("OpenAI", "DeepSeek", "Qwen", "Open Router", "Mistral", "OpenRouter")
        for (fk in fallbackKeys) {
            val entry = keys.find { it.provider.equals(fk, ignoreCase = true) }
            if (entry != null) {
                val baseUrl = providerBaseUrls.value[entry.provider] ?: resolveDefaultBaseUrlForProvider(entry.provider)
                return EmbeddingKeyInfo(entry.provider, entry.key, baseUrl)
            }
        }
        val first = keys.firstOrNull() ?: return null
        return EmbeddingKeyInfo(first.provider, first.key, resolveDefaultBaseUrlForProvider(first.provider))
    }

    private fun resolveDefaultBaseUrlForProvider(provider: String): String {
        return when (provider.lowercase()) {
            "openai" -> "https://api.openai.com/v1"
            "mistral" -> "https://api.mistral.ai/v1"
            "deepseek" -> "https://api.deepseek.com/v1"
            "qwen" -> "https://dashscope-intl.aliyuncs.com/compatible-mode/v1"
            "open router", "openrouter" -> "https://openrouter.ai/api/v1"
            else -> "https://api.openai.com/v1"
        }
    }

    private fun resolveEmbeddingBaseUrl(): String {
        return providerBaseUrls.value["OpenAI"] ?: "https://api.openai.com/v1"
    }

    fun indexMessageForRag(messageId: String, text: String) {
        viewModelScope.launch(Dispatchers.IO) {
            val model = activeEmbeddingModel.value
            if (model == null) {
                DebugLog.d("AgoraVM", "RAG index: no active model, skipping $messageId")
                return@launch
            }
            DebugLog.d("AgoraVM", "RAG index: indexing $messageId with model '${model.name}'")
            val toEmbed = text.take(Constants.MAX_EMBEDDING_TEXT_LENGTH)
            val embedding: FloatArray? = if (model.type == EmbeddingModelType.LOCAL) {
                if (!LlamaEngine.isModelReady(model.localFilePath)) {
                    DebugLog.w("AgoraVM", "RAG index: local model not ready, skipping")
                    return@launch
                }
                LlamaEngine.computeEmbedding(toEmbed, model.localFilePath) {
                    localProvider.releaseEngineBlocking()
                }
            } else {
                val apiKey = model.remoteApiKey.ifBlank { resolveEmbeddingApiKey() ?: "" }
                if (apiKey.isBlank()) {
                    DebugLog.w("AgoraVM", "RAG index: no API key, skipping")
                    return@launch
                }
                val baseUrl = model.remoteBaseUrl.ifBlank { resolveEmbeddingBaseUrl() }
                EmbeddingClient.computeEmbedding(toEmbed, apiKey, model.remoteModelName, baseUrl)
            }
            if (embedding != null) {
                chatDao.upsertEmbedding(EmbeddingEntity(
                    messageId = messageId,
                    modelId = model.id,
                    embedding = EmbeddingIndexer.floatsToBytes(embedding),
                    chunkText = text.take(Constants.MAX_CHUNK_TEXT_LENGTH),
                    dimension = embedding.size
                ))
                DebugLog.d("AgoraVM", "RAG index: stored embedding (dim=${embedding.size}) for $messageId")
            }
        }
    }
    suspend fun searchMessages(query: String, limit: Int = 20) = chatDao.searchMessages(query, limit)
    fun setAppLanguage(language: String) { viewModelScope.launch { settingsManager.saveAppLanguage(language) } }
    fun setWebSearchEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveWebSearchEnabled(enabled) } }
    fun setWebSearchProvider(provider: String) { viewModelScope.launch { settingsManager.saveWebSearchProvider(provider) } }
    fun setWebSearchApiKey(provider: String, apiKey: String) { viewModelScope.launch { settingsManager.saveWebSearchApiKey(provider, apiKey) } }
    fun setWebSearchNumResults(n: Int) { viewModelScope.launch { settingsManager.saveWebSearchNumResults(n) } }
    fun setWebSearchBaseUrl(url: String) { viewModelScope.launch { settingsManager.saveWebSearchBaseUrl(url) } }
    fun setShowDocumentationFab(enabled: Boolean) { viewModelScope.launch { settingsManager.saveShowDocumentationFab(enabled) } }
    fun setShellEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveShellEnabled(enabled) } }
    fun setTorEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveTorEnabled(enabled) } }
    fun setTorSocksPort(port: Int) { viewModelScope.launch { settingsManager.saveTorSocksPort(port) } }
    fun setThinkingEnabled(enabled: Boolean) { viewModelScope.launch { settingsManager.saveThinkingEnabled(enabled) } }
    fun setThinkingLevel(level: String) { viewModelScope.launch { settingsManager.saveThinkingLevel(level) } }
    fun setDefaultTemperature(v: Float?) { viewModelScope.launch { settingsManager.saveDefaultTemperature(v) } }
    fun setDefaultMaxTokens(v: Int?) { viewModelScope.launch { settingsManager.saveDefaultMaxTokens(v) } }
    fun setDefaultTopP(v: Float?) { viewModelScope.launch { settingsManager.saveDefaultTopP(v) } }
    fun setDefaultFrequencyPenalty(v: Float?) { viewModelScope.launch { settingsManager.saveDefaultFrequencyPenalty(v) } }
    fun setDefaultPresencePenalty(v: Float?) { viewModelScope.launch { settingsManager.saveDefaultPresencePenalty(v) } }
    fun setConversationSettings(convId: String, settings: ConversationSettings?) {
        viewModelScope.launch { settingsManager.saveConversationSettings(convId, settings) }
    }
    fun buildEffectiveConversationSettings(conversationId: String): ConversationSettings {
        val overrides = conversationSettings.value[conversationId]
            ?: _pendingConversationSettings.value  // new chat: may not be saved to map yet
            ?: ConversationSettings()
        return ConversationSettings(
            contextWindow = overrides.contextWindow ?: maxContextWindow.value,
            temperature = overrides.temperature ?: defaultTemperature.value,
            maxTokens = overrides.maxTokens ?: defaultMaxTokens.value,
            topP = overrides.topP ?: defaultTopP.value,
            frequencyPenalty = overrides.frequencyPenalty ?: defaultFrequencyPenalty.value,
            presencePenalty = overrides.presencePenalty ?: defaultPresencePenalty.value,
            codeExecutionEnabled = overrides.codeExecutionEnabled ?: codeExecutionEnabled.value,
            googleSearchEnabled = overrides.googleSearchEnabled ?: googleSearchEnabled.value,
            thinkingEnabled = overrides.thinkingEnabled ?: thinkingEnabled.value,
            thinkingLevel = overrides.thinkingLevel ?: thinkingLevel.value,
            webSearchEnabled = if (webSearchEnabled.value) (overrides.webSearchEnabled ?: true) else false,
            shellEnabled = if (shellEnabled.value) (overrides.shellEnabled ?: true) else false
        )
    }

    private fun buildGenerationPair(
        providerName: String,
        modelId: String,
        activeKey: String,
        resolvedSystemPrompt: String?,
        resolvedUserPrepend: String?,
        resolvedUserPostpend: String?,
        effectiveSettings: ConversationSettings,
        currentId: String
    ): Pair<GenerationConfig, GenerationContext> {
        val config = GenerationConfig(
            providerName = providerName,
            modelId = modelId.substringAfter(":"),
            apiKey = activeKey,
            effectiveSystemPrompt = resolvedSystemPrompt,
            maxContextWindow = effectiveSettings.contextWindow ?: maxContextWindow.value,
            codeExecutionEnabled = effectiveSettings.codeExecutionEnabled ?: codeExecutionEnabled.value,
            googleSearchEnabled = effectiveSettings.googleSearchEnabled ?: googleSearchEnabled.value,
            thinkingEnabled = effectiveSettings.thinkingEnabled ?: thinkingEnabled.value,
            thinkingLevel = effectiveSettings.thinkingLevel ?: thinkingLevel.value,
            baseUrl = getEffectiveBaseUrl(providerName),
            userPrepend = resolvedUserPrepend,
            userPostpend = resolvedUserPostpend,
            temperature = effectiveSettings.temperature,
            maxTokens = effectiveSettings.maxTokens,
            topP = effectiveSettings.topP,
            frequencyPenalty = effectiveSettings.frequencyPenalty,
            presencePenalty = effectiveSettings.presencePenalty
        )
        val genCtx = com.newoether.agora.viewmodel.GenerationContext(
            conversationId = currentId,
            accessSavedMemories = accessSavedMemories.value,
            accessActiveMemory = accessActiveMemory.value,
            accessPastConversations = accessPastConversations.value,
            modelSearchMethod = modelSearchMethod.value,
            activeEmbeddingConfig = activeEmbeddingModel.value,
            embeddingApiKey = resolveEmbeddingApiKey() ?: "",
            ragThreshold = ragThreshold.value,
            searchMatchLimit = searchMatchLimit.value,
            searchContextWindow = searchContextWindow.value,
            webSearchEnabled = effectiveSettings.webSearchEnabled ?: webSearchEnabled.value,
            webSearchApiKeys = webSearchApiKeys.value,
            webSearchProvider = webSearchProvider.value,
            webSearchNumResults = webSearchNumResults.value,
            webSearchBaseUrl = webSearchBaseUrl.value,
            shellEnabled = effectiveSettings.shellEnabled ?: shellEnabled.value,
            shellDevices = shellDevices.value,
            imageTranscriptionEnabled = imageTranscriptionEnabledModels.value.contains(currentActiveModel.value),
            imageTranscriptionModel = imageTranscriptionModel.value,
            imageTranscriptionBatchSize = imageTranscriptionBatchSize.value,
            transcriptionProviderName = resolveTranscriptionProviderName(),
            transcriptionModelId = resolveTranscriptionModelId(),
            transcriptionApiKey = resolveTranscriptionApiKey(),
            transcriptionBaseUrl = resolveTranscriptionBaseUrl()
        )
        return Pair(config, genCtx)
    }

    private fun onStreamClear() {
        val msg = _streamingMessage.value
        if (msg?.status != MessageStatus.STOPPED) {
            if (msg != null) { _allMessages.update { it.map { m -> if (m.id == msg.id) msg else m } } }
            _streamingMessage.value = null
        }
        val id = activeEmbeddingModelId.value
        if (id.isNotEmpty()) cacheMessagesForModel(id, silent = true)
    }

    fun addShellDevice(device: com.newoether.agora.data.ShellDeviceConfig) {
        viewModelScope.launch {
            val current = shellDevices.value.toMutableList()
            current.add(device)
            settingsManager.saveShellDevices(current)
        }
    }
    fun updateShellDevice(device: com.newoether.agora.data.ShellDeviceConfig) {
        viewModelScope.launch {
            val current = shellDevices.value.toMutableList()
            val idx = current.indexOfFirst { it.id == device.id }
            if (idx >= 0) { current[idx] = device; settingsManager.saveShellDevices(current) }
        }
    }
    fun setThemeMode(mode: String) { viewModelScope.launch { settingsManager.saveThemeMode(mode) } }
    fun setColorScheme(scheme: String) { viewModelScope.launch { settingsManager.saveColorScheme(scheme) } }
    fun setDynamicColor(enabled: Boolean) { viewModelScope.launch { settingsManager.saveDynamicColor(enabled) } }
    fun setSchemeStyle(style: String) { viewModelScope.launch { settingsManager.saveSchemeStyle(style) } }
    fun removeShellDevice(deviceId: String) {
        viewModelScope.launch {
            settingsManager.saveShellDevices(shellDevices.value.filter { it.id != deviceId })
        }
    }
    fun setSearchMatchLimit(n: Int) { viewModelScope.launch { settingsManager.saveSearchMatchLimit(n) } }
    fun setSearchContextWindow(n: Int) { viewModelScope.launch { settingsManager.saveSearchContextWindow(n) } }
    fun setRagThreshold(threshold: Float) { viewModelScope.launch { settingsManager.saveRagThreshold(threshold) } }
    suspend fun testRemoteEmbedding(modelName: String, baseUrl: String, apiKey: String = ""): String? {
        val effectiveKey = apiKey.ifBlank { resolveEmbeddingApiKey() ?: "" }
        val url = baseUrl.ifBlank { resolveEmbeddingBaseUrl() }
        return withContext(Dispatchers.IO) {
            try {
                val result = EmbeddingClient.computeEmbedding("test connection", effectiveKey, modelName, url)
                if (result != null) "OK (dim=${result.size})" else "Request failed. Check API key, URL, and model name."
            } catch (e: Exception) {
                e.message ?: "Error"
            }
        }
    }

    fun createNewChat() {
        switchingJob?.cancel()
        if (!_isNewChatMode.value) {
            _pendingSystemPromptId.value = null
        }
        _isNewChatMode.value = true
        _isTransitioningToNewChat.value = true
        _isSwitching.value = true
        switchingJob = viewModelScope.launch {
            kotlinx.coroutines.delay(200) // Allow overlay to fade in
            clearMessageHeights()
            _currentConversationId.value = null
            _currentActiveModel.value = null
            _pendingConversationSettings.value = null
            _allMessages.value = emptyList()
            _selectedChildren.value = emptyMap()
            _branchSwitchTrigger.value = null
            _isSwitching.value = false
            _isTransitioningToNewChat.value = false
        }
    }

    fun selectConversation(id: String) {
        if (_currentConversationId.value == id && !_isNewChatMode.value) return

        switchingJob?.cancel()
        _isTransitioningToNewChat.value = false
        _isSwitching.value = true
        switchingJob = viewModelScope.launch {
            kotlinx.coroutines.delay(200) // Allow overlay to fade in
            _isNewChatMode.value = false
            clearMessageHeights()
            _branchSwitchTrigger.value = null
            _currentConversationId.value = id
            val conversation = chatDao.getConversation(id)
            _currentActiveModel.value = conversation?.modelId
            triggerScrollToMessage()
        }
    }

    fun renameConversation(id: String, newTitle: String) {
        viewModelScope.launch {
            val existing = chatDao.getConversation(id)
            if (existing != null) {
                chatDao.upsertConversation(existing.copy(title = newTitle))
            }
        }
    }

    fun generateTitle(conversationId: String) {
        viewModelScope.launch {
            _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.snackbar_generating_title)))
            val conversation = chatDao.getConversation(conversationId) ?: return@launch
            val path = messages.value
            val firstUserMsg = path.firstOrNull { it.participant == Participant.USER } ?: return@launch
            val firstModelMsg = path
                .filter { it.participant == Participant.MODEL && it.text.isNotBlank() }
                .firstOrNull()

            val titleModelId = titleGenerationModel.value
            val modelIdWithPrefix = if (!titleModelId.isNullOrBlank()) titleModelId else (conversation.modelId ?: firstModelMsg?.modelName ?: selectedModel.value)
            val providerName = getProviderForModel(modelIdWithPrefix)
            val modelId = modelIdWithPrefix.substringAfter(":")
            val activeKeyId = activeApiKeyIds.value[providerName]
            val activeKey = apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
            if (!isProviderConfigured(providerName, activeKey)) {
                emitSnackbar(getApplication<Application>().getString(R.string.no_api_key_for_provider, providerName))
                return@launch
            }

            val summaryText = if (firstModelMsg != null) {
                "User: ${firstUserMsg.text}\nAssistant: ${firstModelMsg.text.take(500)}"
            } else {
                firstUserMsg.text
            }

            val titlePrompt = listOf(
                ChatMessage(
                    text = "Generate a short title (5 words maximum) for this conversation:\n\n$summaryText\n\nRespond with ONLY the title text, no quotes, no punctuation, no explanation.",
                    participant = Participant.USER,
                    status = MessageStatus.SUCCESS
                )
            )

            val provider = getProviderInstance(providerName)
            val config = ProviderConfig(
                apiKey = activeKey,
                modelId = modelId,
                systemPrompt = "You are a title generator. Output only a short title in the same language as the conversation.",
                maxContextWindow = 1,
                thinkingEnabled = false,
                baseUrl = getEffectiveBaseUrl(providerName)
            )

            var title = ""
            try {
                // Serialize with embedding to avoid dual model load OOM
                if (providerName == "Local") {
                    LlamaEngine.modelMutex.withLock {
                        withContext(Dispatchers.IO) {
                            provider.generateResponse(titlePrompt, config).collect { event ->
                                if (event is StreamEvent.TextChunk) title += event.text
                                else if (event is StreamEvent.Error) DebugLog.e("AgoraVM", "Title generation error: ${event.message}")
                            }
                        }
                        localProvider.releaseEngine()
                    }
                } else {
                    provider.generateResponse(titlePrompt, config).collect { event ->
                        if (event is StreamEvent.TextChunk) title += event.text
                        else if (event is StreamEvent.Error) DebugLog.e("AgoraVM", "Title generation error: ${event.message}")
                    }
                }
            } catch (e: Exception) {
                DebugLog.e("AgoraVM", "Title generation failed for provider=$providerName model=$modelId", e)
                return@launch
            }

            title = title.trim().replace("\n", " ").take(60)
            if (title.isNotBlank()) {
                renameConversation(conversationId, title)
                _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.snackbar_title_generated)))
            } else {
                _snackbarMessage.emit(SnackbarEvent(appContext.getString(R.string.snackbar_title_error)))
            }
        }
    }

    fun setConversationSystemPrompt(id: String, promptId: String?) {
        viewModelScope.launch {
            val existing = chatDao.getConversation(id)
            if (existing != null) {
                chatDao.upsertConversation(existing.copy(systemPromptId = promptId))
            }
        }
    }

    fun setActiveModel(model: String) {
        _currentActiveModel.value = model
        _currentConversationId.value?.let { id ->
            viewModelScope.launch {
                val existing = chatDao.getConversation(id)
                if (existing != null) {
                    chatDao.upsertConversation(existing.copy(modelId = model))
                }
            }
        }
    }

    fun deleteConversation(id: String) {
        if (_currentConversationId.value == id) {
            stopGeneration()
        }
        viewModelScope.launch(Dispatchers.IO) {
            // Delete attachment files for all messages in this conversation
            val allMsgs = chatDao.getMessagesForConversation(id).first()
            for (msg in allMsgs) {
                for (imagePath in msg.images) {
                    try { java.io.File(imagePath).delete() } catch (_: Exception) {}
                }
                // Delete video files referenced in attachmentMeta
                if (msg.attachmentMeta != null) {
                    try {
                        val meta = kotlinx.serialization.json.Json.decodeFromString<com.newoether.agora.model.AttachmentMeta>(msg.attachmentMeta!!)
                        for (item in meta.items) {
                            if (item.type == "video" && item.originalUri?.startsWith("file://") == true) {
                                try {
                                    java.io.File(item.originalUri.removePrefix("file://")).delete()
                                } catch (_: Exception) {}
                            }
                        }
                    } catch (_: Exception) {}
                }
            }
            chatDao.deleteEmbeddingsByConversation(id)
            chatDao.deleteMessagesByConversation(id)
            chatDao.deleteConversation(id)
            if (_currentConversationId.value == id) createNewChat()
        }
    }

    /**
     * Deletes a message and all its descendants (BFS cascade).
     * Hidden tool_/result_ children are included in the cascade.
     * Attachments, embeddings, and branch selections are cleaned up.
     * Returns the count of deleted messages (for the confirmation dialog).
     */
    fun deleteMessage(messageId: String): Int {
        val currentId = _currentConversationId.value ?: return 0
        stopGeneration()

        val allMsgs = _allMessages.value
        val targetMsg = allMsgs.find { it.id == messageId } ?: return 0

        // BFS walk to collect all descendant IDs (including hidden tool_/result_ messages)
        val staleIds = linkedSetOf(messageId)
        val queue = mutableListOf(messageId)
        while (queue.isNotEmpty()) {
            val pid = queue.removeAt(0)
            allMsgs.filter { it.parentId == pid }.forEach {
                if (staleIds.add(it.id)) queue.add(it.id)
            }
        }

        viewModelScope.launch(Dispatchers.IO) {
            // Delete attachment files for all cascaded messages
            val staleList = allMsgs.filter { it.id in staleIds }
            for (msg in staleList) {
                for (imagePath in msg.images) {
                    try { java.io.File(imagePath).delete() } catch (_: Exception) {}
                }
                if (msg.attachmentMeta != null) {
                    for (item in msg.attachmentMeta.items) {
                        if ((item.type == "video" || item.type == "image" || item.type == "file") && item.originalUri?.startsWith("file://") == true) {
                            try { java.io.File(item.originalUri.removePrefix("file://")).delete() } catch (_: Exception) {}
                        }
                    }
                }
            }

            // Delete embeddings for all cascaded messages
            for (id in staleIds) {
                chatDao.deleteEmbedding(id)
            }

            // DB delete
            chatDao.deleteMessagesByIds(staleIds.toList())

            // Update _allMessages
            _allMessages.update { it.filter { m -> m.id !in staleIds } }

            // Fix _selectedChildren — remove entries where key or value is deleted.
            // If a deleted message was the selected branch, switch to the next available sibling.
            val remainingMsgs = _allMessages.value
            val newSelected = _selectedChildren.value.toMutableMap()
            var changed = false
            for ((parentId, childId) in _selectedChildren.value) {
                // Remove entry if the parent itself was deleted
                if (parentId != null && parentId in staleIds) {
                    newSelected.remove(parentId)
                    changed = true
                    continue
                }
                if (childId in staleIds) {
                    val siblings = remainingMsgs.filter {
                        it.parentId == parentId &&
                            !it.id.startsWith(Constants.TOOL_MSG_PREFIX) &&
                            !it.id.startsWith(Constants.RESULT_MSG_PREFIX)
                    }.sortedBy { it.timestamp }
                    if (siblings.isNotEmpty()) {
                        newSelected[parentId] = siblings.last().id
                    } else {
                        newSelected.remove(parentId)
                    }
                    changed = true
                }
            }
            if (changed) _selectedChildren.value = newSelected
        }

        return staleIds.size
    }

    fun stopGeneration() {
        generationJob?.cancel()
        _isLoading.value = false
        val stoppedMsg = _streamingMessage.value?.copy(status = MessageStatus.STOPPED)
        _streamingMessage.value = stoppedMsg
        // Also update _allMessages entity immediately so UI reacts without flowOn delay
        if (stoppedMsg != null) {
            _allMessages.update { it.map { m ->
                if (m.id == stoppedMsg.id) stoppedMsg else m
            } }
        } else {
            // _streamingMessage was null — find the in-flight model message directly
            _allMessages.update { it.map { m ->
                if (m.participant == Participant.MODEL &&
                    (m.status == MessageStatus.SENDING || m.status == MessageStatus.THINKING || m.status == MessageStatus.TOOL_CALLING || m.status == MessageStatus.TRANSCRIBING)
                ) m.copy(status = MessageStatus.STOPPED) else m
            } }
        }
        _generatingInConversationId.value = null
        AgoraForegroundService.stop(getApplication())
    }

    fun regenerate(messageId: String) {
        val currentId = _currentConversationId.value ?: return
        val modelId = currentActiveModel.value
        val providerName = getProviderForModel(modelId)
        val activeKeyId = activeApiKeyIds.value[providerName]
        val activeKey = apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
        if (!isProviderConfigured(providerName, activeKey)) {
            emitSnackbar(getApplication<Application>().getString(R.string.no_api_key_for_provider, providerName))
            return
        }

        stopGeneration()

        // Compute IDs and set placeholder on the calling thread before launching IO work,
        // so the combine function never sees _streamingMessage=null while the error is in _allMessages.
        val messageToRegenerate = _allMessages.value.find { it.id == messageId } ?: return
        val parentId = messageToRegenerate.parentId ?: return
        val isErrorOrStopped = messageToRegenerate.status == MessageStatus.ERROR || messageToRegenerate.status == MessageStatus.STOPPED
        val isLatest = _allMessages.value.none { it.parentId == messageId && !it.id.startsWith(Constants.TOOL_MSG_PREFIX) && !it.id.startsWith(Constants.RESULT_MSG_PREFIX) }
        // Error/stopped: purge and replace in-place. Normal: create new branch.
        val modelMessageId = if (isErrorOrStopped && isLatest) messageId else UUID.randomUUID().toString()
        val startTime = System.currentTimeMillis() + 1

        // Insert placeholder into _allMessages and update _selectedChildren on the calling
        // thread BEFORE setting _streamingMessage. This ensures the combine function sees a
        // consistent state where the new ID is both present and selected, avoiding a frame
        // where two model messages appear in the path.
        val placeholder = ChatMessage(
            id = modelMessageId, parentId = parentId, text = "", participant = Participant.MODEL,
            status = MessageStatus.SENDING, timestamp = startTime
        )
        _allMessages.update { it.filter { m -> m.id != modelMessageId } + placeholder }
        val newMap = _selectedChildren.value.toMutableMap()
        newMap[parentId] = modelMessageId
        _selectedChildren.value = newMap

        _streamingMessage.value = placeholder
        _isLoading.value = true

        generationJob = generationScope.launch {
            try {
                val userMessage = _allMessages.value.find { it.id == parentId } ?: return@launch

            if (isErrorOrStopped && isLatest) {
                // Purge stale tool call children, thinking content, and embeddings
                val allMsgs = _allMessages.value
                val staleIds = mutableListOf<String>()
                val queue = mutableListOf(modelMessageId)
                while (queue.isNotEmpty()) {
                    val pid = queue.removeAt(0)
                    allMsgs.filter { it.parentId == pid && (it.id.startsWith(Constants.TOOL_MSG_PREFIX) || it.id.startsWith(Constants.RESULT_MSG_PREFIX)) }
                        .forEach { staleIds.add(it.id); queue.add(it.id) }
                }
                if (staleIds.isNotEmpty()) {
                    chatDao.deleteMessagesByIds(staleIds)
                    _allMessages.update { it.filter { m -> m.id !in staleIds } }
                }
                chatDao.deleteEmbedding(modelMessageId)
                chatDao.upsertMessage(MessageEntity(
                    id = modelMessageId, conversationId = currentId, parentId = parentId,
                    text = "", thoughts = null, thoughtTitle = null, status = MessageStatus.SENDING, participant = Participant.MODEL, timestamp = startTime,
                    modelName = currentActiveModel.value, toolCallJson = null
                ))
            } else {
                // New branch — old message and its tool calls stay as a selectable branch
                chatDao.upsertMessage(MessageEntity(
                    id = modelMessageId, conversationId = currentId, parentId = parentId,
                    text = "", thoughts = null, status = MessageStatus.SENDING, participant = Participant.MODEL, timestamp = startTime,
                    modelName = currentActiveModel.value
                ))
            }
            chatDao.getConversation(currentId)?.let { conv ->
                chatDao.upsertConversation(conv.copy(lastUpdated = System.currentTimeMillis()))
            }
            val resolved = buildEffectiveSystemPrompt(currentId)
            val effectiveSettings = buildEffectiveConversationSettings(currentId)
            val (config, genCtx) = buildGenerationPair(
                providerName, modelId, activeKey,
                resolved.systemPrompt, resolved.userPrepend, resolved.userPostpend,
                effectiveSettings, currentId
            )
            generationManager.generate(
                conversationId = currentId,
                modelMessageId = modelMessageId,
                startTime = startTime,
                isRegenerate = true,
                replaceMessageId = messageId,
                modelName = currentActiveModel.value,
                config = config,
                ctx = genCtx,
                generationJob = generationJob,
                onStreamUpdate = { _streamingMessage.value = it },
                onLoadingChange = { _isLoading.value = it },
                onGeneratingIdChange = { _generatingInConversationId.value = it },
                onStreamClear = { onStreamClear() }
            )
            } finally {
                if (_isLoading.value && generationJob === currentCoroutineContext()[Job]) _isLoading.value = false
            }
        }
    }

    fun switchBranch(parentId: String?, direction: Int) {
        if (_isLoading.value) return
        val siblings = _allMessages.value.filter { it.parentId == parentId && !it.id.startsWith(Constants.TOOL_MSG_PREFIX) && !it.id.startsWith(Constants.RESULT_MSG_PREFIX) }.sortedBy { it.timestamp }
        if (siblings.size < 2) return
        val currentId = _selectedChildren.value[parentId] ?: siblings.last().id
        val currentIndex = siblings.indexOfFirst { it.id == currentId }
        val newIndex = (currentIndex + direction).coerceIn(0, siblings.size - 1)
        
        switchingJob?.cancel()
        _isSwitching.value = true
        switchingJob = viewModelScope.launch {
            kotlinx.coroutines.delay(200) // Allow overlay to fade in
            val newMap = _selectedChildren.value.toMutableMap()
            val targetMessage = siblings[newIndex]
            newMap[parentId] = targetMessage.id
            _selectedChildren.value = newMap
            
            _branchSwitchTrigger.value = targetMessage.id
            triggerScrollToMessage(targetMessage.id)
        }
    }

    fun editMessage(messageId: String, newText: String) {
        val currentId = _currentConversationId.value ?: return
        val modelId = currentActiveModel.value
        val providerName = getProviderForModel(modelId)
        val activeKeyId = activeApiKeyIds.value[providerName]
        val activeKey = apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
        if (!isProviderConfigured(providerName, activeKey)) {
            emitSnackbar(getApplication<Application>().getString(R.string.no_api_key_for_provider, providerName))
            return
        }

        stopGeneration()
        generationJob = generationScope.launch {
            val messageToEdit = _allMessages.value.find { it.id == messageId } ?: return@launch
            val newUserMessageId = UUID.randomUUID().toString()
            chatDao.upsertMessage(MessageEntity(
                id = newUserMessageId, conversationId = currentId, parentId = messageToEdit.parentId,
                text = newText, thoughts = null, status = MessageStatus.SUCCESS, participant = Participant.USER, timestamp = System.currentTimeMillis()
            ))
            val newMap = _selectedChildren.value.toMutableMap()
            newMap[messageToEdit.parentId] = newUserMessageId
            _selectedChildren.value = newMap
            val modelMessageId = UUID.randomUUID().toString()
            val startTime = System.currentTimeMillis() + 1
            chatDao.upsertMessage(MessageEntity(
                id = modelMessageId, conversationId = currentId, parentId = newUserMessageId,
                text = "", thoughts = null, status = MessageStatus.SENDING, participant = Participant.MODEL, timestamp = startTime,
                modelName = currentActiveModel.value
            ))
            chatDao.getConversation(currentId)?.let { conv ->
                chatDao.upsertConversation(conv.copy(lastUpdated = System.currentTimeMillis()))
            }
            val resolved = buildEffectiveSystemPrompt(currentId)
            val effectiveSettings = buildEffectiveConversationSettings(currentId)
            val (config, genCtx) = buildGenerationPair(
                providerName, modelId, activeKey,
                resolved.systemPrompt, resolved.userPrepend, resolved.userPostpend,
                effectiveSettings, currentId
            )
            generationManager.generate(
                conversationId = currentId,
                modelMessageId = modelMessageId,
                startTime = startTime,
                isRegenerate = false,
                replaceMessageId = null,
                modelName = currentActiveModel.value,
                config = config,
                ctx = genCtx,
                generationJob = generationJob,
                onStreamUpdate = { _streamingMessage.value = it },
                onLoadingChange = { _isLoading.value = it },
                onGeneratingIdChange = { _generatingInConversationId.value = it },
                onStreamClear = { onStreamClear() }
            )
        }
    }

    private data class ResolvedPrompt(
        val systemPrompt: String?,
        val userPrepend: String?,
        val userPostpend: String?
    )

    private suspend fun buildEffectiveSystemPrompt(currentId: String): ResolvedPrompt {
        val conversation = chatDao.getConversation(currentId)
        val targetPromptId = conversation?.systemPromptId ?: activeSystemPromptId.value
        val entry = systemPrompts.value.find { it.id == targetPromptId }
        val activeMemory = memoryManager.getActiveMemory()
        val includeActiveMemory = accessActiveMemory.value
        val modelId = currentActiveModel.value.substringAfter(":")

        val sdf = java.text.SimpleDateFormat("HH:mm:ss", java.util.Locale.US)
        val dateSdf = java.text.SimpleDateFormat("yyyy-MM-dd", java.util.Locale.US)
        val now = java.util.Date()

        val runtimeValues = mapOf(
            PredefinedVariables.TIME to sdf.format(now),
            PredefinedVariables.DATE to dateSdf.format(now),
            PredefinedVariables.SENT_TIME to sdf.format(now),
            PredefinedVariables.SENT_DATE to dateSdf.format(now),
            PredefinedVariables.MODEL_ID to modelId,
            PredefinedVariables.ACTIVE_MEMORY to if (includeActiveMemory && activeMemory.isNotBlank()) activeMemory else ""
        )

        if (entry != null) {
            val systemItems = entry.resolvedSystemItems
            // Prepend/postpend: {sent_time}/{sent_date} stay as placeholders resolved per-message in applyUserTemplate
            val perMsgValues = runtimeValues.filterKeys { it !in PredefinedVariables.PER_MESSAGE_VARS }
            return ResolvedPrompt(
                systemPrompt = PredefinedVariables.compile(systemItems, runtimeValues).ifBlank { null },
                userPrepend = PredefinedVariables.compile(entry.userPrependItems, perMsgValues, emptyMap()).ifBlank { null },
                userPostpend = PredefinedVariables.compile(entry.userPostpendItems, perMsgValues, emptyMap()).ifBlank { null }
            )
        }

        return ResolvedPrompt(null, null, null)
    }

    private fun getFileName(context: android.content.Context, uri: android.net.Uri): String {
        return try {
            val cursor = context.contentResolver.query(uri, arrayOf(android.provider.OpenableColumns.DISPLAY_NAME), null, null, null)
            cursor?.use {
                if (it.moveToFirst()) {
                    val idx = it.getColumnIndex(android.provider.OpenableColumns.DISPLAY_NAME)
                    if (idx >= 0) it.getString(idx) ?: uri.lastPathSegment ?: "unknown"
                    else uri.lastPathSegment ?: "unknown"
                } else uri.lastPathSegment ?: "unknown"
            } ?: (uri.lastPathSegment ?: "unknown")
        } catch (_: Exception) {
            uri.lastPathSegment ?: "unknown"
        }
    }

    fun sendMessage(text: String, images: List<String> = emptyList(), attachments: List<SelectedAttachment> = emptyList()): Boolean {
        if (!sendGate.compareAndSet(false, true)) return false
        var committed = false
        try {
        val modelId = currentActiveModel.value
        if (modelId.isBlank()) {
            emitSnackbar(getApplication<Application>().getString(R.string.no_model_selected))
            return false
        }
        val providerName = getProviderForModel(modelId)
        val activeKeyId = activeApiKeyIds.value[providerName]
        val activeKey = apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
        if (!isProviderConfigured(providerName, activeKey)) {
            emitSnackbar(getApplication<Application>().getString(R.string.no_api_key_for_provider, providerName))
            return false
        }
        if (providerName == "Local") {
            val localModelId = modelId.substringAfter("Local:")
            val config = localChatModels.value.find { it.modelId == localModelId }
            if (config == null || !java.io.File(config.localFilePath).exists()) {
                emitSnackbar(getApplication<Application>().getString(R.string.local_model_not_found))
                return false
            }
        }
        stopGeneration()

        committed = true
        generationJob = generationScope.launch {
            try {
            val app = getApplication<Application>()
            // mediaUris: URIs that need processImages (images, video content:// URIs)
            // directPaths: paths that skip processImages (pre-extracted frames, PDF copies, rendered pages)
            val mediaUris = mutableListOf<String>()
            val directPaths = mutableListOf<String>()
            val sliceConfigs = mutableMapOf<String, GenerationManager.VideoSliceConfig>()
            val metaItems = mutableListOf<com.newoether.agora.model.AttachmentItem>()
            var nextImageIndex = 0

            // Process legacy images list (backward compatibility)
            for (uri in images) {
                val mimeType = try { app.contentResolver.getType(android.net.Uri.parse(uri)) } catch (_: Exception) { null }
                if (mimeType != null && !mimeType.startsWith("image/") && !mimeType.startsWith("video/")) {
                    mediaUris.add(uri)
                    try {
                        val isText = mimeType.startsWith("text/") || mimeType == "application/json" || mimeType == "application/xml"
                        if (isText) {
                            app.contentResolver.openInputStream(android.net.Uri.parse(uri))?.use { stream ->
                                val content = stream.bufferedReader().readText().take(Constants.MAX_FILE_CONTENT_READ_LENGTH)
                                if (content.isNotBlank()) {
                                    val fileName = getFileName(app, android.net.Uri.parse(uri))
                                    // Legacy file content stored in text (pre-attachmentMeta)
                                }
                            }
                        }
                    } catch (_: Exception) {}
                } else {
                    mediaUris.add(uri)
                }
            }

            // Process new SelectedAttachment list
            for (att in attachments) {
                when (att.type) {
                    "image" -> {
                        mediaUris.add(att.uri)
                        metaItems.add(com.newoether.agora.model.AttachmentItem(
                            originalUri = att.uri, type = "image", mimeType = att.mimeType,
                            imageIndex = nextImageIndex
                        ))
                        nextImageIndex++
                    }
                    "video" -> {
                        // Copy video to local storage for export/playback survival
                        val videoExt = when {
                            att.mimeType?.contains("mp4") == true -> "mp4"
                            att.mimeType?.contains("webm") == true -> "webm"
                            att.mimeType?.contains("quicktime") == true -> "mov"
                            else -> "mp4"
                        }
                        val videoFile = java.io.File(app.filesDir, "vid_original_${java.util.UUID.randomUUID()}.$videoExt")
                        var localVideoUri: String? = null
                        try {
                            app.contentResolver.openInputStream(android.net.Uri.parse(att.uri))?.use { input ->
                                videoFile.outputStream().use { input.copyTo(it) }
                            }
                            localVideoUri = "file://${videoFile.absolutePath}"
                        } catch (_: Exception) {
                            // Fallback: keep original content URI (may expire)
                            localVideoUri = att.uri
                        }

                        if (att.processedFrames != null && att.processedFrames.isNotEmpty()) {
                            metaItems.add(com.newoether.agora.model.AttachmentItem(
                                originalUri = localVideoUri, type = "video",
                                fileName = att.fileName, mimeType = att.mimeType,
                                imageIndex = nextImageIndex, pageCount = att.frameCount
                            ))
                            directPaths.addAll(att.processedFrames)
                            nextImageIndex += att.processedFrames.size
                        } else {
                            val frameCount = att.frameCount ?: 1
                            metaItems.add(com.newoether.agora.model.AttachmentItem(
                                originalUri = localVideoUri, type = "video",
                                fileName = att.fileName, mimeType = att.mimeType,
                                imageIndex = nextImageIndex, pageCount = att.frameCount
                            ))
                            mediaUris.add(att.uri)
                            if (att.frameCount != null && att.frameCount > 1 && att.sliceIntervalMs != null) {
                                sliceConfigs[att.uri] = GenerationManager.VideoSliceConfig(
                                    intervalMicros = att.sliceIntervalMs * 1000L,
                                    frameCount = att.frameCount
                                )
                            }
                            nextImageIndex += frameCount
                        }
                    }
                    "file" -> {
                        var textContent: String? = null
                        try {
                            app.contentResolver.openInputStream(android.net.Uri.parse(att.uri))?.use { stream ->
                                val content = stream.bufferedReader().readText().take(Constants.MAX_FILE_CONTENT_READ_LENGTH)
                                if (content.isNotBlank()) {
                                    val fileName = att.fileName ?: getFileName(app, android.net.Uri.parse(att.uri))
                                    textContent = content
                                }
                            }
                        } catch (_: Exception) {}
                        metaItems.add(com.newoether.agora.model.AttachmentItem(
                            originalUri = att.uri, type = "file",
                            fileName = att.fileName, mimeType = att.mimeType,
                            textContent = textContent
                        ))
                    }
                    "pdf" -> {
                        val pagePaths = if (att.preRenderedPaths != null && att.preRenderedPaths.isNotEmpty()) {
                            val sel = att.selectedPages ?: att.preRenderedPaths.indices.toSet()
                            att.preRenderedPaths.filterIndexed { i, _ -> i in sel }
                        } else {
                            com.newoether.agora.util.PdfPageRenderer.renderAsImages(app, android.net.Uri.parse(att.uri), att.selectedPages)
                        }
                        if (pagePaths.isEmpty()) {
                            _snackbarMessage.emit(SnackbarEvent(app.getString(R.string.pdf_render_failed)))
                            continue
                        }
                        metaItems.add(com.newoether.agora.model.AttachmentItem(
                            originalUri = att.uri, type = "pdf",
                            fileName = att.fileName, mimeType = "application/pdf",
                            imageIndex = nextImageIndex, pageCount = pagePaths.size
                        ))
                        directPaths.addAll(pagePaths)
                        nextImageIndex += pagePaths.size
                    }
                }
            }

            val finalText = text
            val processedImages = if (mediaUris.isNotEmpty()) generationManager.processImages(mediaUris, sliceConfigs) else emptyList()
            val allImages = processedImages + directPaths

            // Recalculate imageIndex for all meta items based on final allImages positions.
            // nextImageIndex tracked the expected order:
            //   First N items correspond to mediaUris entries (→ processedImages)
            //   Remaining items correspond to directPaths entries
            // After processing, processedImages may differ in size from mediaUris.
            // We build a position map: for each metaItem that has imageIndex < mediaUris.size,
            // it was tracking an offset within mediaUris. We need the actual offset within processedImages.
            val uriToResultMap = mutableListOf<IntRange>() // for each mediaUris entry, the range in processedImages
            var pos = 0
            for (uri in mediaUris) {
                val start = pos
                // Count consecutive results belonging to this URI by scanning forward until
                // we find files that don't correspond. Since we can't distinguish, use a simple
                // heuristic: each URI produces either 0 or 1+ results. The slice configs tell us
                // how many frames per video.
                val config = sliceConfigs[uri]
                val expectedCount = config?.frameCount ?: 1
                val end = minOf(pos + expectedCount, processedImages.size)
                uriToResultMap.add(start until end)
                pos = end
            }
            // Cap at processedImages size
            val adjustedMetaItems = metaItems.map { item ->
                val idx = item.imageIndex
                if (idx == null) {
                    item
                } else if (idx < mediaUris.size && idx < uriToResultMap.size) {
                    val range = uriToResultMap[idx]
                    item.copy(imageIndex = range.first)
                } else if (idx in mediaUris.size until (mediaUris.size + directPaths.size)) {
                    // This item's imageIndex is relative to directPaths start
                    item.copy(imageIndex = processedImages.size + (idx - mediaUris.size))
                } else {
                    // Fallback: keep original index (shouldn't happen for well-formed input)
                    item
                }
            }
            val attachmentMeta = if (adjustedMetaItems.isNotEmpty()) {
                com.newoether.agora.model.AttachmentMeta(items = adjustedMetaItems)
            } else null
            var currentId = _currentConversationId.value
            val wasNewChat = _isNewChatMode.value
            if (wasNewChat) {
                val newId = UUID.randomUUID().toString()
                chatDao.upsertConversation(ChatEntity(id = newId, title = appContext.getString(R.string.new_chat), modelId = currentActiveModel.value, systemPromptId = _pendingSystemPromptId.value))
                _currentConversationId.value = newId
                _isNewChatMode.value = false
                currentId = newId
            }
            if (currentId == null) {
                val newId = UUID.randomUUID().toString()
                chatDao.upsertConversation(ChatEntity(id = newId, title = appContext.getString(R.string.new_chat), modelId = currentActiveModel.value, systemPromptId = _pendingSystemPromptId.value))
                _currentConversationId.value = newId
                _isNewChatMode.value = false
                currentId = newId
            }
            // Apply pending per-conversation settings if any (from Advanced dialog in new chat)
            val pendingSettings = _pendingConversationSettings.value
            if (pendingSettings != null && currentId != null) {
                viewModelScope.launch { settingsManager.saveConversationSettings(currentId, pendingSettings) }
                _pendingConversationSettings.value = null
            }
            val currentPath = messages.value
            val lastMessageId = currentPath.lastOrNull()?.id
            val userMessageId = UUID.randomUUID().toString()
            chatDao.upsertMessage(MessageEntity(
                id = userMessageId, conversationId = currentId, parentId = lastMessageId,
                text = finalText, images = allImages, thoughts = null, status = MessageStatus.SUCCESS, participant = Participant.USER, timestamp = System.currentTimeMillis(),
                attachmentMeta = attachmentMeta?.let { kotlinx.serialization.json.Json.encodeToString(it) }
            ))
            settingsManager.incrementMessagesSent()
            val modelMessageId = UUID.randomUUID().toString()
            val startTime = System.currentTimeMillis() + 1
            chatDao.upsertMessage(MessageEntity(
                id = modelMessageId, conversationId = currentId, parentId = userMessageId,
                text = "", thoughts = null, status = MessageStatus.SENDING, participant = Participant.MODEL, timestamp = startTime,
                modelName = currentActiveModel.value
            ))
            chatDao.getConversation(currentId)?.let { conv ->
                chatDao.upsertConversation(conv.copy(lastUpdated = System.currentTimeMillis()))
            }
            triggerScrollToMessage(userMessageId)

            val resolved = buildEffectiveSystemPrompt(currentId)
            val effectiveSettings = buildEffectiveConversationSettings(currentId)
            val (config, genCtx) = buildGenerationPair(
                providerName, modelId, activeKey,
                resolved.systemPrompt, resolved.userPrepend, resolved.userPostpend,
                effectiveSettings, currentId
            )
            try {
                generationManager.generate(
                    conversationId = currentId,
                    modelMessageId = modelMessageId,
                    startTime = startTime,
                    isRegenerate = false,
                    replaceMessageId = null,
                    modelName = currentActiveModel.value,
                    config = config,
                    ctx = genCtx,
                    generationJob = generationJob,
                    onStreamUpdate = { _streamingMessage.value = it },
                    onLoadingChange = { _isLoading.value = it },
                    onGeneratingIdChange = { _generatingInConversationId.value = it },
                    onStreamClear = { onStreamClear() }
                )
            } catch (e: CancellationException) {
                throw e
            } catch (_: Exception) {
            }

            val lastMsg = _allMessages.value.find { it.id == modelMessageId }
            if (wasNewChat && titleGenerationEnabled.value && generationJob?.isActive == true && lastMsg?.status != MessageStatus.ERROR) {
                generateTitle(currentId)
            }
        } finally {
            sendGate.set(false)
        }
        } // end launch
    } finally {
        if (!committed) sendGate.set(false)
    }
        return true
    }

    fun fetchAvailableModels() {
        viewModelScope.launch {
            if (_isSyncingModels.value) return@launch
            _isSyncingModels.value = true
            val successProviders = mutableListOf<String>()
            val failedProviders = mutableListOf<String>()
            var skippedCount = 0

            // Ensure custom providers are loaded into the providers map before iterating
            customProviders.value.forEach { config ->
                if (config.name !in providers) {
                    val url = providerBaseUrls.value[config.name] ?: ""
                    providers[config.name] = CustomOpenAiProvider(config.name, url)
                }
            }

            val message = try {
                providers.forEach { (name, providerInstance) ->
                    if (name == "Local") return@forEach

                    try {
                        val activeKeyId = activeApiKeyIds.value[name]
                        val activeKey = apiKeys.value.find { it.id == activeKeyId }?.key ?: ""
                        val isCustomProvider = name !in builtInProviders
                        val currentBaseUrl = if (isCustomProvider) {
                            providerBaseUrls.value[name]?.takeIf { it.isNotBlank() } ?: providerInstance.defaultBaseUrl
                        } else {
                            providerBaseUrls.value[name]
                        }

                        if (!isProviderConfigured(name, activeKey)) {
                            skippedCount++
                            settingsManager.saveAvailableModels(name, emptyList())
                            return@forEach
                        }

                        val rawModels = withTimeout(10_000L) {
                            providerInstance.fetchModels(activeKey, currentBaseUrl)
                        }
                        if (rawModels.isNotEmpty()) {
                            val prefixedModels = rawModels.map { "$name:${it.removePrefix("models/")}" }
                            settingsManager.saveAvailableModels(name, prefixedModels)
                            successProviders.add(name)
                        } else {
                            failedProviders.add(name)
                        }
                    } catch (e: Exception) {
                        failedProviders.add(name)
                    }
                }

                val allFetchedModels = settingsManager.availableModels.first().values.flatten().toSet()
                val newEnabled = enabledModels.value.intersect(allFetchedModels)
                setEnabledModels(newEnabled)

                when {
                    successProviders.isNotEmpty() && failedProviders.isEmpty() ->
                        appContext.getString(R.string.sync_success_providers, successProviders.size)
                    successProviders.isNotEmpty() && failedProviders.isNotEmpty() ->
                        appContext.getString(R.string.sync_partial, successProviders.joinToString(), failedProviders.joinToString())
                    successProviders.isEmpty() && failedProviders.isNotEmpty() ->
                        appContext.getString(R.string.sync_failed_providers, failedProviders.joinToString())
                    else -> if (skippedCount > 0) appContext.getString(R.string.sync_no_providers) else appContext.getString(R.string.sync_completed)
                }
            } catch (e: Exception) {
                appContext.getString(R.string.sync_failed_providers, e.message ?: appContext.getString(R.string.unknown_error))
            } finally {
                _isSyncingModels.value = false
            }

            _snackbarMessage.tryEmit(SnackbarEvent(message))
        }
    }

    // ---- Data Control: Export / Import ----

    fun refreshDataCounts() {
        viewModelScope.launch(Dispatchers.IO) {
            _conversationCount.value = chatDao.getAllConversationsList().size
            _memoryCount.value = memoryManager.listFiles().size +
                (if (memoryManager.getActiveMemory().isNotEmpty()) 1 else 0)
            _systemPromptCount.value = settingsManager.systemPrompts.first().size
        }
    }

    fun exportData(uri: Uri, categories: Set<DataExporter.ExportCategory>, includeApiKeys: Boolean) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val exporter = DataExporter(getApplication(), chatDao, settingsManager, memoryManager)
                exporter.export(uri, categories, includeApiKeys) { progress ->
                    _exportProgress.value = progress
                }
                _exportProgress.value = null
                _snackbarMessage.emit(SnackbarEvent(getApplication<android.app.Application>().getString(R.string.export_success)))
            } catch (e: Exception) {
                _exportProgress.value = null
                _snackbarMessage.emit(SnackbarEvent(
                    getApplication<android.app.Application>().getString(R.string.export_failed, e.localizedMessage ?: "")
                ))
            }
        }
    }

    fun previewImport(uri: Uri) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val importer = DataImporter(getApplication(), chatDao, settingsManager, memoryManager)
                val manifest = importer.readManifest(uri)
                if (manifest == null) {
                    _snackbarMessage.emit(SnackbarEvent(
                        getApplication<android.app.Application>().getString(R.string.import_invalid_file)
                    ))
                    return@launch
                }
                val preview = importer.preview(uri)
                if (preview.conversationCount == 0 && preview.memoryCount == 0 &&
                    preview.systemPromptCount == 0 && !preview.settingsPresent) {
                    _snackbarMessage.emit(SnackbarEvent(
                        getApplication<android.app.Application>().getString(R.string.import_no_data)
                    ))
                    return@launch
                }
                _importManifest.value = manifest
                _importPreview.value = preview
            } catch (e: Exception) {
                _snackbarMessage.emit(SnackbarEvent(
                    getApplication<android.app.Application>().getString(R.string.import_failed, e.localizedMessage ?: "")
                ))
            }
        }
    }

    fun clearImportState() {
        _importManifest.value = null
        _importPreview.value = null
    }

    fun setClaudeImportPreview(preview: ClaudeChatImporter.ImportPreview) {
        _claudeImportPreview.value = preview
    }

    fun previewClaudeChat(json: String) {
        try {
            val importer = ClaudeChatImporter()
            val parseResult = importer.parseJson(json)
            if (parseResult.isSuccess) {
                val preview = importer.preview(parseResult.getOrThrow())
                _claudeImportPreview.value = preview
            } else {
                emitSnackbar(parseResult.exceptionOrNull()?.localizedMessage ?: appContext.getString(R.string.parse_error))
                _claudeImportPreview.value = null
            }
        } catch (e: Exception) {
            emitSnackbar(e.localizedMessage ?: appContext.getString(R.string.unknown_error))
            _claudeImportPreview.value = null
        }
    }

    fun setClaudeImportError(error: String) {
        emitSnackbar(error)
        _claudeImportPreview.value = null
    }

    fun clearClaudeImportState() {
        _claudeImportPreview.value = null
        _claudeImportProgress.value = null
        _claudeImportResult.value = null
    }

    fun importClaudeChat(uri: Uri, strategy: com.newoether.agora.ui.settings.ImportStrategy, selectedIds: Set<String>) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                _claudeImportProgress.value = 0.2f
                val bytes = getApplication<android.app.Application>().contentResolver.openInputStream(uri)?.use { it.readBytes() }
                if (bytes == null) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.claude_import_error_detail, appContext.getString(R.string.could_not_read_file)))
                    return@launch
                }

                val importer = ClaudeChatImporter()
                val jsonResult = importer.extractJsonFromBytes(bytes)
                if (jsonResult.isFailure) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.claude_import_error_detail, jsonResult.exceptionOrNull()?.localizedMessage ?: appContext.getString(R.string.parse_error)))
                    return@launch
                }

                val parseResult = importer.parseJson(jsonResult.getOrThrow())
                if (parseResult.isFailure) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.claude_import_error_detail, parseResult.exceptionOrNull()?.localizedMessage ?: appContext.getString(R.string.parse_error)))
                    return@launch
                }

                _claudeImportProgress.value = 0.4f
                val conversations = parseResult.getOrThrow()
                val preview = importer.preview(conversations)
                val importData = importer.toImportFormat(conversations, selectedIds)

                if (preview.totalMessageCount == 0) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.claude_import_no_data))
                    return@launch
                }

                _claudeImportProgress.value = 0.6f

                // Convert to Room entities
                val chatEntities = importData.conversations.map { ce ->
                    ChatEntity(ce.id, ce.title, ce.lastUpdated, ce.selectedBranchesJson, ce.systemPromptId, ce.modelId)
                }
                val messageEntities = importData.messages.map { me ->
                    MessageEntity(
                        id = me.id, conversationId = me.conversationId, parentId = me.parentId,
                        text = me.text, images = me.images, thoughts = me.thoughts,
                        thoughtTitle = me.thoughtTitle, tokenCount = me.tokenCount,
                        status = safeValueOf<MessageStatus>(me.status) ?: MessageStatus.SUCCESS,
                        participant = safeValueOf<Participant>(me.participant) ?: Participant.MODEL,
                        timestamp = me.timestamp, thoughtTimeMs = me.thoughtTimeMs,
                        modelName = me.modelName, toolCallJson = me.toolCallJson,
                        attachmentMeta = me.attachmentMeta
                    )
                }

                if (strategy == com.newoether.agora.ui.settings.ImportStrategy.REPLACE) {
                    chatDao.deleteAllConversations()
                    chatEntities.forEach { chatDao.upsertConversation(it) }
                    messageEntities.forEach { chatDao.upsertMessage(it) }
                    _claudeImportProgress.value = 0.8f
                    _claudeImportResult.value = ClaudeChatImporter.ImportResult(chatEntities.size, messageEntities.size)
                } else {
                    val existingConvIds = chatDao.getAllConversationsList().map { it.id }.toSet()
                    val existingMsgIds = chatDao.findExistingMessageIds(messageEntities.map { it.id }).toSet()
                    val newCh = chatEntities.filterNot { it.id in existingConvIds }
                    val newMsgs = messageEntities.filterNot { it.id in existingMsgIds }
                    newCh.forEach { chatDao.upsertConversation(it) }
                    newMsgs.forEach { chatDao.upsertMessage(it) }
                    _claudeImportProgress.value = 0.8f
                    _claudeImportResult.value = ClaudeChatImporter.ImportResult(newCh.size, newMsgs.size)
                }
                _claudeImportProgress.value = null
                refreshDataCounts()
            } catch (e: Exception) {
                _claudeImportProgress.value = null
                emitSnackbar(getApplication<android.app.Application>().getString(R.string.claude_import_error_detail, e.localizedMessage ?: ""))
            }
        }
    }

    fun previewGptChat(bytes: ByteArray) {
        try {
            val importer = GptChatImporter()
            val parseResult = importer.extractAndParse(bytes)
            if (parseResult.isSuccess) {
                val conversations = parseResult.getOrThrow()
                val preview = importer.preview(conversations)
                _gptImportPreview.value = preview
            } else {
                emitSnackbar(parseResult.exceptionOrNull()?.localizedMessage ?: appContext.getString(R.string.parse_error))
                _gptImportPreview.value = null
            }
        } catch (e: Exception) {
            emitSnackbar(e.localizedMessage ?: appContext.getString(R.string.unknown_error))
            _gptImportPreview.value = null
        }
    }

    fun setGptImportError(error: String) {
        emitSnackbar(error)
        _gptImportPreview.value = null
    }

    fun clearGptImportState() {
        _gptImportPreview.value = null
        _gptImportProgress.value = null
        _gptImportResult.value = null
    }

    fun importGptChat(uri: Uri, strategy: com.newoether.agora.ui.settings.ImportStrategy, selectedIds: Set<String>) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                _gptImportProgress.value = 0.2f
                val bytes = getApplication<android.app.Application>().contentResolver.openInputStream(uri)?.use { it.readBytes() }
                if (bytes == null) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.gpt_import_error_detail, appContext.getString(R.string.could_not_read_file)))
                    return@launch
                }

                val importer = GptChatImporter()
                val parseResult = importer.extractAndParse(bytes)
                if (parseResult.isFailure) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.gpt_import_error_detail, parseResult.exceptionOrNull()?.localizedMessage ?: appContext.getString(R.string.parse_error)))
                    return@launch
                }

                _gptImportProgress.value = 0.4f
                val conversations = parseResult.getOrThrow()
                val preview = importer.preview(conversations)
                val importData = importer.toImportFormat(conversations, selectedIds)

                if (preview.totalMessageCount == 0) {
                    emitSnackbar(getApplication<android.app.Application>().getString(R.string.gpt_import_no_data))
                    return@launch
                }

                _gptImportProgress.value = 0.6f

                val chatEntities = importData.conversations.map { ce ->
                    ChatEntity(ce.id, ce.title, ce.lastUpdated, ce.selectedBranchesJson, ce.systemPromptId, ce.modelId)
                }
                val messageEntities = importData.messages.map { me ->
                    MessageEntity(
                        id = me.id, conversationId = me.conversationId, parentId = me.parentId,
                        text = me.text, images = me.images, thoughts = me.thoughts,
                        thoughtTitle = me.thoughtTitle, tokenCount = me.tokenCount,
                        status = safeValueOf<MessageStatus>(me.status) ?: MessageStatus.SUCCESS,
                        participant = safeValueOf<Participant>(me.participant) ?: Participant.MODEL,
                        timestamp = me.timestamp, thoughtTimeMs = me.thoughtTimeMs,
                        modelName = me.modelName, toolCallJson = me.toolCallJson,
                        attachmentMeta = me.attachmentMeta
                    )
                }

                val thoughtsCount = importData.messages.count { it.thoughts != null && it.thoughts.isNotBlank() }
                if (strategy == com.newoether.agora.ui.settings.ImportStrategy.REPLACE) {
                    chatDao.deleteAllConversations()
                    chatEntities.forEach { chatDao.upsertConversation(it) }
                    messageEntities.forEach { chatDao.upsertMessage(it) }
                    _gptImportProgress.value = 0.8f
                    _gptImportResult.value = GptChatImporter.ImportResult(chatEntities.size, messageEntities.size, thoughtsCount)
                } else {
                    val existingConvIds = chatDao.getAllConversationsList().map { it.id }.toSet()
                    val existingMsgIds = chatDao.findExistingMessageIds(messageEntities.map { it.id }).toSet()
                    val newCh = chatEntities.filterNot { it.id in existingConvIds }
                    val newMsgs = messageEntities.filterNot { it.id in existingMsgIds }
                    val newThoughtsCount = newMsgs.count { it.thoughts != null && it.thoughts.isNotBlank() }
                    newCh.forEach { chatDao.upsertConversation(it) }
                    newMsgs.forEach { chatDao.upsertMessage(it) }
                    _gptImportProgress.value = 0.8f
                    _gptImportResult.value = GptChatImporter.ImportResult(newCh.size, newMsgs.size, newThoughtsCount)
                }
                _gptImportProgress.value = null
                refreshDataCounts()
            } catch (e: Exception) {
                _gptImportProgress.value = null
                emitSnackbar(getApplication<android.app.Application>().getString(R.string.gpt_import_error_detail, e.localizedMessage ?: ""))
            }
        }
    }

    fun importData(uri: Uri, decisions: Map<DataExporter.ExportCategory, DataImporter.ImportStrategy>) {
        viewModelScope.launch(Dispatchers.IO) {
            try {
                val importer = DataImporter(getApplication(), chatDao, settingsManager, memoryManager)
                val result = importer.import(uri, decisions) { progress ->
                    _importProgress.value = progress
                }
                _importProgress.value = null
                _importManifest.value = null
                _importPreview.value = null
                refreshDataCounts()

                val parts = mutableListOf<String>()
                if (result.conversationsImported > 0) parts.add("${result.conversationsImported} conversations")
                if (result.memoriesImported > 0) parts.add("${result.memoriesImported} memories")
                if (result.systemPromptsImported > 0) parts.add("${result.systemPromptsImported} prompts")
                if (result.settingsImported) parts.add("settings")
                if (result.apiKeysImported) parts.add("API keys")

                val summary = if (result.errors.isEmpty()) {
                    getApplication<android.app.Application>().getString(R.string.import_success, parts.joinToString(", "))
                } else {
                    getApplication<android.app.Application>().getString(R.string.import_failed,
                        "${result.errors.size} error(s): ${result.errors.first()}")
                }
                _snackbarMessage.emit(SnackbarEvent(summary))
            } catch (e: Exception) {
                _importProgress.value = null
                _snackbarMessage.emit(SnackbarEvent(
                    getApplication<android.app.Application>().getString(R.string.import_failed, e.localizedMessage ?: "")
                ))
            }
        }
    }
}
