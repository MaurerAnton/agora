package com.newoether.agora.data

import android.content.Context
import android.net.Uri
import com.newoether.agora.automation.LoopPolicy
import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.deleteSemanticModel
import com.newoether.agora.data.local.invalidateSemanticModel
import com.newoether.agora.data.local.semanticModelSnapshot
import com.newoether.agora.data.local.TaskEntity
import com.newoether.agora.data.repository.ConversationSettingsTransferCoordinator
import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.NonCancellable
import kotlinx.coroutines.flow.first
import kotlinx.coroutines.sync.withLock
import kotlinx.coroutines.withContext
import kotlinx.serialization.ExperimentalSerializationApi
import kotlinx.serialization.Serializable
import kotlinx.serialization.SerialName
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import java.io.File
import java.io.IOException
import java.util.UUID

/**
 * Imported automations are content, not permission to spend tokens in the background. Preserve a
 * valid cron for the user to review, but always restore the task disabled with no armed epoch.
 */
internal fun sanitizeImportedTask(task: TaskEntity): TaskEntity {
    val cron = task.cronExpr.trim()
    return task.copy(
        name = task.name.trim(),
        prompt = task.prompt.trim(),
        cronExpr = cron,
        nextRunAt = 0L,
        enabled = false,
    )
}

/**
 * Converts legacy unbounded loops to the bounded default. Invalid cadence/cycle state is kept
 * visible for diagnostics where useful, but is always made inactive so it cannot be scheduled.
 */
internal fun sanitizeImportedLoop(loop: LoopEntity): LoopEntity {
    val importedMaxCycles = loop.maxCycles
    val maxCycles = importedMaxCycles
        ?.takeIf { it in LoopPolicy.MIN_MAX_CYCLES..LoopPolicy.MAX_MAX_CYCLES }
        ?: LoopPolicy.DEFAULT_MAX_CYCLES
    return loop.copy(
        prompt = LoopPolicy.normalizePrompt(loop.prompt),
        cycleCount = loop.cycleCount.coerceAtLeast(0),
        maxCycles = maxCycles,
        // Importing a backup never authorizes an automatic model call. Keep the state for review,
        // but require an explicit restart on this device.
        active = false,
        nextFireAt = 0L,
    )
}

/** Prevents a missing Task row from making an imported execution permanently unreachable. */
internal fun sanitizeImportedConversation(
    conversation: ChatEntity,
    availableTaskIds: Set<String>,
): ChatEntity {
    val withoutDeviceReadState = conversation.copy(hasUnreadGeneration = false)
    return if (
        withoutDeviceReadState.taskId != null &&
        withoutDeviceReadState.taskId !in availableTaskIds
    ) {
        withoutDeviceReadState.copy(taskId = null, origin = "user", graduated = true)
    } else {
        withoutDeviceReadState
    }
}

internal fun embeddingModelSemanticsChanged(
    before: EmbeddingModelConfig,
    after: EmbeddingModelConfig,
): Boolean = before.type != after.type || when (after.type) {
    EmbeddingModelType.REMOTE ->
        before.remoteModelName != after.remoteModelName ||
            before.remoteBaseUrl != after.remoteBaseUrl
    EmbeddingModelType.LOCAL -> before.localFilePath != after.localFilePath
}

class DataImporter(
    private val context: Context,
    private val database: ChatDatabase,
    private val chatDao: ChatDao,
    private val settingsManager: SettingsManager,
    private val memoryManager: MemoryManager,
    private val skillManager: SkillManager,
    private val conversationSettingsTransfers: ConversationSettingsTransferCoordinator,
) {
    private val importJson = Json { ignoreUnknownKeys = true; coerceInputValues = true }
    enum class ImportStrategy { MERGE, REPLACE, SKIP }

    companion object {
        private const val MAX_CUSTOM_FONT_BYTES = 64L * 1024L * 1024L
    }

    private val importJson = Json { ignoreUnknownKeys = true }
    private val conversationMediaRestorer = NativeConversationMediaRestorer(context, importJson)
    private val conversationGraphImporter = NativeConversationGraphImporter(
        database = database,
        chatDao = chatDao,
        importJson = importJson,
        mediaRestorer = conversationMediaRestorer,
    )

    private suspend fun reconcileImportedEmbeddingModels(
        previousModels: List<EmbeddingModelConfig>,
    ) {
        val previousById = previousModels.associateBy(EmbeddingModelConfig::id)
        val importedModelIds = settingsManager.embeddingModels.first().map(EmbeddingModelConfig::id)
        val affectedModelIds = buildSet {
            addAll(previousById.keys)
            addAll(importedModelIds)
        }
        val updatedAt = System.currentTimeMillis()
        affectedModelIds.forEach { modelId ->
            EmbeddingCacheLocks.forModel(modelId).withLock {
                val current = settingsManager.embeddingModels.first()
                    .firstOrNull { it.id == modelId }
                val previous = previousById[modelId]
                when {
                    current == null -> database.deleteSemanticModel(modelId)
                    previous == null || embeddingModelSemanticsChanged(previous, current) ->
                        database.invalidateSemanticModel(modelId, updatedAt)
                }
            }
        }
    }

    @Serializable
    data class ImportManifest(
        @SerialName("agora_export_version") val version: Int = 1,
        @SerialName("app_version") val appVersion: String = "",
        @SerialName("exported_at") val exportedAt: String = "",
        val categories: List<String> = emptyList(),
        @SerialName("has_api_keys") val hasApiKeys: Boolean = false
    )

    data class ImportPreview(
        val manifest: ImportManifest,
        val conversationCount: Int = 0,
        val taskCount: Int = 0,
        val loopCount: Int = 0,
        val memoryCount: Int = 0,
        val systemPromptCount: Int = 0,
        val settingsPresent: Boolean = false,
        val apiKeysPresent: Boolean = false
    ) {
        val hasConversationGraph: Boolean
            get() = conversationCount > 0 || taskCount > 0 || loopCount > 0
        val hasImportableData: Boolean
            get() = hasConversationGraph || memoryCount > 0 || systemPromptCount > 0 ||
                settingsPresent || apiKeysPresent
        val isSupportedVersion: Boolean
            get() = NativeBackupFormat.isSupported(manifest.version)
    }

    data class ImportResult(
        val conversationsImported: Int = 0,
        val tasksImported: Int = 0,
        val loopsImported: Int = 0,
        val memoriesImported: Int = 0,
        val systemPromptsImported: Int = 0,
        val settingsImported: Boolean = false,
        val apiKeysImported: Boolean = false,
        val errors: List<String> = emptyList()
    )

    private data class PromptImportResult(
        val importedCount: Int = 0,
        val idMap: Map<String, String> = emptyMap(),
        val availableIds: Set<String> = emptySet(),
    ) {
        fun resolve(id: String?): String? =
            id?.let { original -> idMap[original] ?: original.takeIf(availableIds::contains) }
    }


    suspend fun readManifest(uri: Uri): ImportManifest? {
        return withContext(Dispatchers.IO) {
            NativeBackupArchive.open(context, uri)?.use { archive ->
                val manifestJson = archive[NativeBackupFormat.MANIFEST_ENTRY]
                    ?.decodeToString() ?: return@use null
                try {
                    importJson.decodeFromString<ImportManifest>(manifestJson)
                } catch (_: Exception) {
                    null
                }
            }
        }
    }

    @OptIn(ExperimentalSerializationApi::class)
    suspend fun preview(uri: Uri): ImportPreview {
        return withContext(Dispatchers.IO) {
            val empty = ImportPreview(ImportManifest(version = 0))
            val archive = NativeBackupArchive.open(context, uri) ?: return@withContext empty
            archive.use {
                val manifestJson = archive[NativeBackupFormat.MANIFEST_ENTRY]
                    ?.decodeToString() ?: return@use empty
                val manifest = try {
                    importJson.decodeFromString<ImportManifest>(manifestJson)
                } catch (_: Exception) {
                    return@use empty
                }

                var conversationCount = 0
                var taskCount = 0
                var loopCount = 0
                var systemPromptCount = 0
                val memoryCount = archive.names().count { it.startsWith("memories/") }
                val settingsPresent = archive.has(NativeBackupFormat.SETTINGS_ENTRY)
                val apiKeysPresent = archive.has(NativeBackupFormat.SECRETS_ENTRY)

                archive.stream(NativeBackupFormat.CONVERSATIONS_ENTRY)?.use { stream ->
                    try {
                        val counts = conversationGraphImporter.countConversationGraph(stream)
                        conversationCount = counts.conversations
                        taskCount = counts.tasks
                        loopCount = counts.loops
                    } catch (e: Exception) { DebugLog.e("DataImporter", "Failed to parse conversations.json", e) }
                }

                archive[NativeBackupFormat.SYSTEM_PROMPTS_ENTRY]?.let { json ->
                    try {
                        val data = importJson.decodeFromString<List<SystemPromptEntry>>(json.decodeToString())
                        systemPromptCount = data.size
                    } catch (e: Exception) { DebugLog.e("DataImporter", "Failed to parse system_prompts.json", e) }
                }

                ImportPreview(
                    manifest = manifest,
                    conversationCount = conversationCount,
                    taskCount = taskCount,
                    loopCount = loopCount,
                    memoryCount = memoryCount,
                    systemPromptCount = systemPromptCount,
                    settingsPresent = settingsPresent,
                    apiKeysPresent = apiKeysPresent
                )
            }
        }
    }

    private suspend fun importSystemPrompts(
        archive: NativeBackupArchive,
        strategy: ImportStrategy,
    ): PromptImportResult {
        val bytes = archive[NativeBackupFormat.SYSTEM_PROMPTS_ENTRY]
            ?: error("${NativeBackupFormat.SYSTEM_PROMPTS_ENTRY} is missing")
        val imported = importJson.decodeFromString<List<SystemPromptEntry>>(bytes.decodeToString())
        if (strategy == ImportStrategy.REPLACE) {
            settingsManager.saveSystemPrompts(imported)
            return PromptImportResult(
                importedCount = imported.size,
                idMap = imported.associate { it.id to it.id },
                availableIds = imported.mapTo(mutableSetOf()) { it.id },
            )
        }

        val merged = settingsManager.systemPrompts.first().toMutableList()
        val usedTitles = merged.mapTo(mutableSetOf()) { it.title }
        val idMap = mutableMapOf<String, String>()
        for (prompt in imported) {
            val sameId = merged.firstOrNull { it.id == prompt.id }
            if (sameId == prompt) {
                idMap[prompt.id] = sameId.id
                continue
            }

            val targetId = if (sameId == null) prompt.id else UUID.randomUUID().toString()
            var targetTitle = prompt.title
            if (targetTitle in usedTitles) {
                val base = "${prompt.title} (imported)"
                targetTitle = base
                var suffix = 2
                while (targetTitle in usedTitles) {
                    targetTitle = "$base $suffix"
                    suffix++
                }
            }
            val restored = prompt.copy(id = targetId, title = targetTitle)
            merged += restored
            usedTitles += targetTitle
            idMap[prompt.id] = targetId
        }
        settingsManager.saveSystemPrompts(merged)
        return PromptImportResult(
            importedCount = imported.size,
            idMap = idMap,
            availableIds = merged.mapTo(mutableSetOf()) { it.id },
        )
    }

    private fun restoreCustomFont(
        archive: NativeBackupArchive,
        archiveVersion: Int,
    ): RestoredCustomFont? {
        val entry = if (archiveVersion >= 4) {
            NativeBackupFormat.CUSTOM_FONT_ENTRY.takeIf(archive::has)
        } else {
            archive.names().firstOrNull { path ->
                path.startsWith("custom_font/") && !path.removePrefix("custom_font/").contains('/')
            }
        } ?: return null
        val declaredSize = archive.size(entry)
        if (declaredSize > MAX_CUSTOM_FONT_BYTES) {
            throw IOException("Custom font exceeds the ${MAX_CUSTOM_FONT_BYTES / (1024 * 1024)} MB limit")
        }

        val temporary = File(context.filesDir, ".custom_font_import_${UUID.randomUUID()}.tmp")
        val target = File(context.filesDir, "custom_font_import_${UUID.randomUUID()}")
        try {
            archive.copyTo(
                name = entry,
                target = temporary,
                maxBytes = MAX_CUSTOM_FONT_BYTES,
            ) ?: return null
            val displayName = com.newoether.agora.util.readFontName(temporary)
            if (!temporary.renameTo(target)) {
                temporary.inputStream().use { input ->
                    NativeBackupArchive.copyStreamToFile(
                        input = input,
                        target = target,
                        declaredSize = temporary.length(),
                        maxBytes = MAX_CUSTOM_FONT_BYTES,
                        sourceName = "custom font",
                    )
                }
                temporary.delete()
            }
            return RestoredCustomFont(target.absolutePath, displayName)
        } catch (error: Exception) {
            temporary.delete()
            target.delete()
            throw error
        }
    }

    @OptIn(ExperimentalSerializationApi::class)
    suspend fun import(
        uri: Uri,
        decisions: Map<DataExporter.ExportCategory, DataImporter.ImportStrategy>,
        onProgress: (Float) -> Unit = {}
    ): ImportResult {
        return withContext(Dispatchers.IO) {
            val archive = NativeBackupArchive.open(context, uri)
                ?: return@withContext ImportResult(errors = listOf("Could not open backup archive"))
            archive.use { opened ->
                val manifest = opened[NativeBackupFormat.MANIFEST_ENTRY]
                    ?.decodeToString()
                    ?.let { raw ->
                        runCatching { importJson.decodeFromString<ImportManifest>(raw) }.getOrNull()
                    }
                    ?: return@withContext ImportResult(
                        errors = listOf("${NativeBackupFormat.MANIFEST_ENTRY} is missing or invalid"),
                    )
                if (!NativeBackupFormat.isSupported(manifest.version)) {
                    return@withContext ImportResult(
                        errors = listOf(
                            "Unsupported backup version ${manifest.version}; this app supports " +
                                "${NativeBackupFormat.MIN_SUPPORTED_VERSION}–" +
                                "${NativeBackupFormat.CURRENT_VERSION}",
                        ),
                    )
                }

                val errors = mutableListOf<String>()
                var conversationsImported = 0
                var tasksImported = 0
                var loopsImported = 0
                var memoriesImported = 0
                var systemPromptsImported = 0
                var settingsImported = false
                var apiKeysImported = false

                val activeCategories = decisions.filter { it.value != ImportStrategy.SKIP }.keys
                val totalSteps = activeCategories.size
                var completed = 0
                fun step() {
                    completed++
                    onProgress(completed.toFloat() / totalSteps.coerceAtLeast(1))
                }

                val keysDecision = decisions[DataExporter.ExportCategory.API_KEYS]
                val promptsDecision = decisions[DataExporter.ExportCategory.SYSTEM_PROMPTS]
                val convDecision = decisions[DataExporter.ExportCategory.CONVERSATIONS]
                val memDecision = decisions[DataExporter.ExportCategory.MEMORIES]
                val settingsDecision = decisions[DataExporter.ExportCategory.SETTINGS]
                val allowLegacySecrets =
                    manifest.version < NativeBackupFormat.CURRENT_VERSION &&
                        keysDecision != null &&
                        keysDecision != ImportStrategy.SKIP

                opened.preflightImportResources(
                    conversationsSelected = convDecision != null &&
                        convDecision != ImportStrategy.SKIP,
                    settingsSelected = settingsDecision != null &&
                        settingsDecision != ImportStrategy.SKIP,
                    archiveVersion = manifest.version,
                    destinationRoot = context.filesDir,
                    customFontLimitBytes = MAX_CUSTOM_FONT_BYTES,
                )

                // Import prompts before conversations/settings so every archived prompt reference
                // can be resolved after MERGE ID collision handling.
                var promptImport = PromptImportResult(
                    availableIds = settingsManager.systemPrompts.first()
                        .mapTo(mutableSetOf()) { it.id },
                )
                if (promptsDecision != null && promptsDecision != ImportStrategy.SKIP) {
                    try {
                        promptImport = importSystemPrompts(opened, promptsDecision)
                        systemPromptsImported = promptImport.importedCount
                    } catch (error: Exception) {
                        errors += "System prompts: ${error.localizedMessage ?: "Unknown error"}"
                    }
                    step()
                }

                if (convDecision != null && convDecision != ImportStrategy.SKIP) {
                    var restoredMedia: NativeConversationMediaRestorer.RestoredMedia? = null
                    var graphCommitted = false
                    try {
                        conversationSettingsTransfers.completePendingImport()
                        val media = conversationMediaRestorer.restoreConversationMedia(opened)
                        restoredMedia = media
                        val headers = opened.stream(NativeBackupFormat.CONVERSATIONS_ENTRY)
                            ?.use { stream ->
                                conversationGraphImporter.readConversationGraphHeaders(
                                    stream = stream,
                                    strategy = convDecision,
                                    restoredMedia = media,
                                    resolveSystemPromptId = promptImport::resolve,
                                )
                            }
                            ?: error("${NativeBackupFormat.CONVERSATIONS_ENTRY} is missing")
                        val semanticSnapshot = semanticModelSnapshot(
                            activeModelId = settingsManager.activeEmbeddingModelId.first(),
                            configuredModelIds = settingsManager.embeddingModels.first().map { it.id },
                        )
                        val settingsTransferId = conversationGraphImporter.importConversationGraph(
                            archive = opened,
                            strategy = convDecision,
                            headers = headers,
                            restoredMedia = media,
                            archiveVersion = manifest.version,
                            semanticSnapshot = semanticSnapshot,
                        )
                        graphCommitted = true
                        conversationsImported = headers.conversations.size
                        tasksImported = headers.tasks.size
                        loopsImported = headers.loops.size
                        try {
                            conversationSettingsTransfers.completeImport(settingsTransferId)
                        } catch (cancelled: kotlinx.coroutines.CancellationException) {
                            throw cancelled
                        } catch (error: Exception) {
                            errors += "Conversation settings: " +
                                (error.localizedMessage ?: "Deferred until next startup")
                        }
                    } catch (cancelled: kotlinx.coroutines.CancellationException) {
                        if (!graphCommitted) {
                            restoredMedia?.createdFiles?.forEach { runCatching { it.delete() } }
                        }
                        throw cancelled
                    } catch (error: Exception) {
                        if (!graphCommitted) {
                            restoredMedia?.createdFiles?.forEach { runCatching { it.delete() } }
                        }
                        errors += "Conversations: ${error.localizedMessage ?: "Unknown error"}"
                    }
                    step()
                }

                if (memDecision != null && memDecision != ImportStrategy.SKIP) {
                    try {
                        val memNames = opened.names().filter { it.startsWith("memories/") }
                        if (memDecision == ImportStrategy.REPLACE) {
                            memoryManager.listFiles().forEach { memoryManager.deleteFile(it.name) }
                            skillManager.listFiles().forEach { skillManager.deleteFile(it.name) }
                            if (memoryManager.getActiveMemory().isNotEmpty()) {
                                memoryManager.updateActiveMemory("", "replace")
                            }
                        }
                        val existingNames = memoryManager.listFiles().map { it.name }.toSet()
                        val existingSkillNames = skillManager.listFiles().map { it.name }.toSet()
                        for (path in memNames) {
                            val text = opened.bytes(path)?.decodeToString() ?: continue
                            when {
                                path == "memories/active_memory.md" && text.isNotBlank() -> {
                                    if (
                                        memDecision == ImportStrategy.REPLACE ||
                                        memoryManager.getActiveMemory().isEmpty()
                                    ) {
                                        memoryManager.updateActiveMemory(text, "replace")
                                    }
                                    memoriesImported++
                                }
                                path == "memories/memory_db/memory_meta.json" -> {
                                    if (
                                        memDecision == ImportStrategy.REPLACE ||
                                        memoryManager.getMetaJson() == "{}"
                                    ) {
                                        memoryManager.saveMetaJson(text)
                                    }
                                }
                                path == "memories/skill_db/skill_meta.json" -> {
                                    if (
                                        memDecision == ImportStrategy.REPLACE ||
                                        skillManager.getMetaJson() == "{}"
                                    ) {
                                        skillManager.saveMetaJson(text)
                                    }
                                }
                                path.startsWith("memories/skill_db/") -> {
                                    val name = path.removePrefix("memories/skill_db/")
                                    if (
                                        memDecision == ImportStrategy.REPLACE ||
                                        name !in existingSkillNames
                                    ) {
                                        try {
                                            skillManager.createFile(name, text)
                                        } catch (_: Exception) {
                                            skillManager.editFile(name, content = text)
                                        }
                                    }
                                    memoriesImported++
                                }
                                path.startsWith("memories/memory_db/") -> {
                                    val name = path.removePrefix("memories/memory_db/")
                                    if (
                                        memDecision == ImportStrategy.REPLACE ||
                                        name !in existingNames
                                    ) {
                                        try {
                                            memoryManager.createFile(name, text)
                                        } catch (_: Exception) {
                                            memoryManager.editFile(name, text)
                                        }
                                    }
                                    memoriesImported++
                                }
                            }
                        }
                    } catch (error: Exception) {
                        errors += "Memories: ${error.localizedMessage ?: "Unknown error"}"
                    }
                    step()
                }

                if (settingsDecision != null && settingsDecision != ImportStrategy.SKIP) {
                    var restoredFont: RestoredCustomFont? = null
                    var fontApplied = false
                    val previousEmbeddingModels = settingsManager.embeddingModels.first()
                    try {
                        val settingsObject = opened[NativeBackupFormat.SETTINGS_ENTRY]
                            ?.decodeToString()
                            ?.let { Json.parseToJsonElement(it).jsonObject }
                            ?: error("${NativeBackupFormat.SETTINGS_ENTRY} is missing")
                        restoredFont = try {
                            restoreCustomFont(opened, manifest.version)
                        } catch (error: Exception) {
                            errors += "Settings: custom font skipped: " +
                                (error.localizedMessage ?: "invalid font file")
                            null
                        }
                        val warnings = PortableSettingsArchive.restoreFromJsonObject(
                            obj = settingsObject,
                            sm = settingsManager,
                            replace = settingsDecision == ImportStrategy.REPLACE,
                            allowLegacySecrets = allowLegacySecrets,
                            restoredCustomFont = restoredFont,
                            resolveSystemPromptId = promptImport::resolve,
                        )
                        warnings.forEach { errors += "Settings: $it" }
                        fontApplied = restoredFont != null && manifest.version >= 4

                        if (manifest.version < 4) {
                            if (restoredFont != null) {
                                settingsManager.saveCustomFontPath(restoredFont.path)
                                settingsManager.saveCustomFontName(restoredFont.displayName)
                                fontApplied = true
                            }
                            opened[NativeBackupFormat.LEGACY_EXTRA_SETTINGS_ENTRY]
                                ?.decodeToString()
                                ?.let { Json.parseToJsonElement(it).jsonObject }
                                ?.let { legacy ->
                                    ExportExtraSettings.restoreLegacyFromJsonObject(
                                        obj = legacy,
                                        sm = settingsManager,
                                        replace = settingsDecision == ImportStrategy.REPLACE,
                                        allowSecrets = allowLegacySecrets,
                                        allowedConversationIds =
                                            chatDao.getAllConversationIds().toSet(),
                                    )
                                }
                            if (
                                settingsManager.fontPreference.first() == "custom" &&
                                settingsManager.customFontPath.first()
                                    .takeIf(String::isNotBlank)
                                    ?.let(::File)
                                    ?.isFile != true
                            ) {
                                settingsManager.saveFontPreference("app_default")
                                settingsManager.saveCustomFontPath("")
                                settingsManager.saveCustomFontName("")
                            }
                        }
                        settingsImported = true
                    } catch (error: Exception) {
                        if (!fontApplied) restoredFont?.path?.let(::File)?.delete()
                        errors += "Settings: ${error.localizedMessage ?: "Unknown error"}"
                    } finally {
                        withContext(NonCancellable) {
                            runCatching {
                                reconcileImportedEmbeddingModels(previousEmbeddingModels)
                            }.exceptionOrNull()?.let { error ->
                                errors += "Settings index: " +
                                    (error.localizedMessage ?: "Could not refresh embeddings")
                            }
                        }
                    }
                    step()
                }

                if (keysDecision != null && keysDecision != ImportStrategy.SKIP) {
                    try {
                        val data = opened[NativeBackupFormat.SECRETS_ENTRY]
                            ?.decodeToString()
                            ?.let { importJson.decodeFromString<NativeBackupSecrets>(it) }
                            ?: error("${NativeBackupFormat.SECRETS_ENTRY} is missing")
                        NativeBackupSecretsPolicy.restore(
                            data = data,
                            sm = settingsManager,
                            replace = keysDecision == ImportStrategy.REPLACE,
                        ).forEach { errors += "API keys: $it" }
                        apiKeysImported = true
                    } catch (error: Exception) {
                        errors += "API keys: ${error.localizedMessage ?: "Unknown error"}"
                    }
                    step()
                }

                onProgress(1f)
                ImportResult(
                    conversationsImported = conversationsImported,
                    tasksImported = tasksImported,
                    loopsImported = loopsImported,
                    memoriesImported = memoriesImported,
                    systemPromptsImported = systemPromptsImported,
                    settingsImported = settingsImported,
                    apiKeysImported = apiKeysImported,
                    errors = errors,
                )
            }
        }
    }

}
