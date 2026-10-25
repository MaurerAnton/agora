// Golden trace driver for data/DataImporter.kt.
//
// DataImporter and every module it touches compile VERBATIM from the app tree (see run_golden.sh):
// the graph chain, archive reader, media restorer, portable settings archive, legacy extra
// settings, secrets policy, memory/skill managers, font reader and the transfer coordinator.
// The substituted boundaries are the Android/Room surface (Context/ContentResolver with the
// archive descriptor path, ParcelFileDescriptor, FileProvider, Room database) and the DataStore
// SettingsManager/SettingsRepository doubles. DataExporter.ExportCategory is lifted byte-for-byte
// by extract_export_category.py so the export root is not pulled in.
//
// Each step runs preview(uri), import(uri, decisions) or a direct FontUtils read against a ZIP
// built from the scenario's ordered entry list. After an import the trace records the filtered
// SettingsManager call log (only the calls DataImporter's own orchestration and the legacy/secrets
// paths make; the archive's scalar restore calls are pinned by portable-settings-archive), the
// settings values DataImporter reads back, memory/skill state, the files tree, the database tables
// and the coordinator's applied imports. Generated UUIDs and wall-clock stamps are masked after
// serialization; the temporary root path is replaced with <root>.
package data.importer.golden

import android.content.ContentResolver
import android.content.Context
import android.content.pm.PackageManager
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DataExporter
import com.newoether.agora.data.DataImporter
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.MemoryManager
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.SkillManager
import com.newoether.agora.data.SystemPromptEntry
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.repository.ConversationRepository
import com.newoether.agora.data.repository.ConversationSettingsTransferCoordinator
import com.newoether.agora.data.repository.SettingsRepository
import com.newoether.agora.util.readFontMeta
import java.io.File
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import java.sql.ResultSet
import java.util.Base64
import java.util.zip.ZipEntry
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json { ignoreUnknownKeys = true }
private val scenarioJson = Json { ignoreUnknownKeys = true }
private val UUID_PATTERN =
    Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
private val EPOCH_MILLIS = Regex("(?<![0-9.])1[7-9][0-9]{11}(?![0-9])")

private val TRACED_SETTINGS_CALLS = setOf(
    "saveSystemPrompts",
    "saveEmbeddingModels",
    "setActiveEmbeddingModelId",
    "saveShellDevices",
    "saveMcpServers",
    "saveCustomProviders",
    "saveApiKeys",
    "saveActiveApiKeyIds",
    "saveWebSearchApiKeys",
    "saveProxyPassword",
    "saveFontPreference",
    "saveCustomFontPath",
    "saveCustomFontName",
    "saveConversationSettings",
    "saveConversationSettingsMap",
    "saveModelAliases",
    "saveCustomModels",
    "resetPortableSettingsForImport",
    "invalidatePortableModelCaches",
)

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

// ---- scenario helpers --------------------------------------------------------------------------

private fun required(root: JsonObject, key: String): JsonElement =
    root[key] ?: error("settings snapshot is missing `$key`")

private inline fun <reified T> value(root: JsonObject, key: String): T =
    json.decodeFromJsonElement(required(root, key))

private inline fun <reified T> nullableValue(root: JsonObject, key: String): T? {
    val element = required(root, key)
    if (element === JsonNull) return null
    return json.decodeFromJsonElement<T>(element)
}

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.stringOrNull(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.stringList(name: String): List<String> =
    (this[name] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()

private fun JsonObject.stringJsonArray(name: String): JsonArray =
    this[name] as? JsonArray ?: JsonArray(emptyList())

// ---- settings snapshot (the data-exporter stand's parser) --------------------------------------

private fun buildSettings(
    root: JsonObject,
    conversationSettings: Map<String, ConversationSettings>,
    systemPrompts: List<SystemPromptEntry>,
    apiKeys: List<ApiKeyEntry>,
    activeApiKeyIds: Map<String, String>,
    webSearchApiKeys: Map<String, String>,
    proxyPassword: String,
): SettingsManager = SettingsManager(
    strings = mapOf(
        "selectedModel" to value<String>(root, "selectedModel"),
        "contextCompactPrompt" to value<String>(root, "contextCompactPrompt"),
        "thinkingLevel" to value<String>(root, "thinkingLevel"),
        "openAiServiceTier" to value<String>(root, "openAiServiceTier"),
        "anthropicCacheTtl" to value<String>(root, "anthropicCacheTtl"),
        "titleGenerationPrompt" to value<String>(root, "titleGenerationPrompt"),
        "modelSearchMethod" to value<String>(root, "modelSearchMethod"),
        "manualSearchMethod" to value<String>(root, "manualSearchMethod"),
        "appLanguage" to value<String>(root, "appLanguage"),
        "webSearchProvider" to value<String>(root, "webSearchProvider"),
        "webSearchBaseUrl" to value<String>(root, "webSearchBaseUrl"),
        "imageGenSize" to value<String>(root, "imageGenSize"),
        "imageTranscriptionPrompt" to value<String>(root, "imageTranscriptionPrompt"),
        "themeMode" to value<String>(root, "themeMode"),
        "colorScheme" to value<String>(root, "colorScheme"),
        "schemeStyle" to value<String>(root, "schemeStyle"),
        "fontPreference" to value<String>(root, "fontPreference"),
        "proxyType" to value<String>(root, "proxyType"),
        "proxyHost" to value<String>(root, "proxyHost"),
        "proxyPort" to value<String>(root, "proxyPort"),
        "proxyUsername" to value<String>(root, "proxyUsername"),
        "proxyBypass" to value<String>(root, "proxyBypass"),
        "toolCallDisplayMode" to value<String>(root, "toolCallDisplayMode"),
        "thinkingSegmentDisplayMode" to value<String>(root, "thinkingSegmentDisplayMode"),
        "activeEmbeddingModelId" to value<String>(root, "activeEmbeddingModelId"),
        "customFontName" to value<String>(root, "customFontName"),
        "customFontPath" to value<String>(root, "customFontPath"),
    ),
    nullableStrings = mapOf(
        "contextCompactModel" to nullableValue<String>(root, "contextCompactModel"),
        "titleGenerationModel" to nullableValue<String>(root, "titleGenerationModel"),
        "imageGenModel" to nullableValue<String>(root, "imageGenModel"),
        "imageTranscriptionModel" to nullableValue<String>(root, "imageTranscriptionModel"),
        "activeSystemPromptId" to nullableValue<String>(root, "activeSystemPromptId"),
    ),
    booleans = mapOf(
        "accessActiveMemory" to value<Boolean>(root, "accessActiveMemory"),
        "accessPastConversations" to value<Boolean>(root, "accessPastConversations"),
        "accessSavedMemories" to value<Boolean>(root, "accessSavedMemories"),
        "accessSkills" to value<Boolean>(root, "accessSkills"),
        "amoledEnabled" to value<Boolean>(root, "amoledEnabled"),
        "anthropicCacheEnabled" to value<Boolean>(root, "anthropicCacheEnabled"),
        "askUserEnabled" to value<Boolean>(root, "askUserEnabled"),
        "autoCacheEnabled" to value<Boolean>(root, "autoCacheEnabled"),
        "autoExpandActiveGroup" to value<Boolean>(root, "autoExpandActiveGroup"),
        "automationToolsEnabled" to value<Boolean>(root, "automationToolsEnabled"),
        "automationWakeLockEnabled" to value<Boolean>(root, "automationWakeLockEnabled"),
        "autoUpdateCheck" to value<Boolean>(root, "autoUpdateCheck"),
        "autoWrapCodeBlocks" to value<Boolean>(root, "autoWrapCodeBlocks"),
        "blurEffectsEnabled" to value<Boolean>(root, "blurEffectsEnabled"),
        "codeExecutionEnabled" to value<Boolean>(root, "codeExecutionEnabled"),
        "contextCompactEnabled" to value<Boolean>(root, "contextCompactEnabled"),
        "contextCompactPreserveSystemPrompt" to value<Boolean>(root, "contextCompactPreserveSystemPrompt"),
        "detailedTokenUsage" to value<Boolean>(root, "detailedTokenUsage"),
        "dynamicColor" to value<Boolean>(root, "dynamicColor"),
        "exactExecutionEnabled" to value<Boolean>(root, "exactExecutionEnabled"),
        "googleSearchEnabled" to value<Boolean>(root, "googleSearchEnabled"),
        "hapticsEnabled" to value<Boolean>(root, "hapticsEnabled"),
        "imageGenEnabled" to value<Boolean>(root, "imageGenEnabled"),
        "imageTranscriptionEnabled" to value<Boolean>(root, "imageTranscriptionEnabled"),
        "openAiResponsesApiEnabled" to value<Boolean>(root, "openAiResponsesApiEnabled"),
        "openAiServiceTierEnabled" to value<Boolean>(root, "openAiServiceTierEnabled"),
        "parseInlineDollarMath" to value<Boolean>(root, "parseInlineDollarMath"),
        "proxyEnabled" to value<Boolean>(root, "proxyEnabled"),
        "ragSearchEnabled" to value<Boolean>(root, "ragSearchEnabled"),
        "reduceMotion" to value<Boolean>(root, "reduceMotion"),
        "shellConfirmEnabled" to value<Boolean>(root, "shellConfirmEnabled"),
        "shellEnabled" to value<Boolean>(root, "shellEnabled"),
        "showDocumentationFab" to value<Boolean>(root, "showDocumentationFab"),
        "showUncachedNotification" to value<Boolean>(root, "showUncachedNotification"),
        "stickToBottom" to value<Boolean>(root, "stickToBottom"),
        "thinkingBudgetEnabled" to value<Boolean>(root, "thinkingBudgetEnabled"),
        "thinkingEnabled" to value<Boolean>(root, "thinkingEnabled"),
        "titleGenerationEnabled" to value<Boolean>(root, "titleGenerationEnabled"),
        "titleGenerationNotificationsEnabled" to value<Boolean>(root, "titleGenerationNotificationsEnabled"),
        "visualizeContextRollout" to value<Boolean>(root, "visualizeContextRollout"),
        "webSearchEnabled" to value<Boolean>(root, "webSearchEnabled"),
    ),
    ints = mapOf(
        "contextCompactRetainCount" to value<Int>(root, "contextCompactRetainCount"),
        "contextCompactThresholdPercent" to value<Int>(root, "contextCompactThresholdPercent"),
        "imageTranscriptionBatchSize" to value<Int>(root, "imageTranscriptionBatchSize"),
        "maxContextWindow" to value<Int>(root, "maxContextWindow"),
        "searchContextWindow" to value<Int>(root, "searchContextWindow"),
        "searchMatchLimit" to value<Int>(root, "searchMatchLimit"),
        "thinkingBudgetTokens" to value<Int>(root, "thinkingBudgetTokens"),
        "webSearchNumResults" to value<Int>(root, "webSearchNumResults"),
    ),
    nullableInts = mapOf(
        "defaultMaxTokens" to nullableValue<Int>(root, "defaultMaxTokens"),
    ),
    floats = mapOf(
        "ragThreshold" to value<Float>(root, "ragThreshold"),
    ),
    nullableFloats = mapOf(
        "defaultTemperature" to nullableValue<Float>(root, "defaultTemperature"),
        "defaultTopP" to nullableValue<Float>(root, "defaultTopP"),
        "defaultFrequencyPenalty" to nullableValue<Float>(root, "defaultFrequencyPenalty"),
        "defaultPresencePenalty" to nullableValue<Float>(root, "defaultPresencePenalty"),
    ),
    stringSets = mapOf(
        "customModels" to value(root, "customModels"),
        "enabledModels" to value(root, "enabledModels"),
        "imageTranscriptionEnabledModels" to value(root, "imageTranscriptionEnabledModels"),
    ),
    stringMaps = mapOf(
        "modelAliases" to value(root, "modelAliases"),
        "providerBaseUrls" to value(root, "providerBaseUrls"),
    ),
    boolMaps = mapOf(
        "modelProviderNames" to value(root, "modelProviderNames"),
    ),
    embeddingModels = value(root, "embeddingModels"),
    shellDevices = value(root, "shellDevices"),
    mcpServers = value(root, "mcpServers"),
    customProviders = value<List<CustomProviderConfig>>(root, "customProviders"),
    conversationSettings = conversationSettings,
    systemPrompts = systemPrompts,
    apiKeys = apiKeys,
    activeApiKeyIds = activeApiKeyIds,
    webSearchApiKeys = webSearchApiKeys,
    proxyPassword = proxyPassword,
)

// ---- archive construction ----------------------------------------------------------------------

private fun writeArchive(entries: JsonArray, target: File) {
    target.parentFile?.mkdirs()
    ZipOutputStream(target.outputStream().buffered()).use { zip ->
        for (element in entries) {
            val entry = element.jsonObject
            val name = entry.string("name")
            val bytes = entry["text"]?.let { (it as JsonPrimitive).content.toByteArray(Charsets.UTF_8) }
                ?: entry["base64"]?.let { Base64.getDecoder().decode((it as JsonPrimitive).content) }
                ?: error("archive entry $name needs text or base64")
            zip.putNextEntry(ZipEntry(name))
            zip.write(bytes)
            zip.closeEntry()
        }
    }
}

// ---- settings view -----------------------------------------------------------------------------

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

private fun promptJson(prompt: SystemPromptEntry): JsonObject = buildJsonObject {
    put("id", prompt.id)
    put("title", prompt.title)
    put("content", prompt.content)
    fun items(name: String, values: List<com.newoether.agora.data.PromptTemplateItem>) {
        put(name, buildJsonArray {
            values.forEach { item ->
                add(buildJsonObject {
                    put("id", item.id)
                    put("type", item.type.name)
                    put("value", item.value)
                })
            }
        })
    }
    items("systemItems", prompt.systemItems)
    items("userItems", prompt.userItems)
    items("assistantItems", prompt.assistantItems)
    items("userPrependItems", prompt.userPrependItems)
    items("userPostpendItems", prompt.userPostpendItems)
}

private fun conversationSettingsJson(settings: ConversationSettings): JsonObject = buildJsonObject {
    put("contextWindow", settings.contextWindow)
    put("temperature", settings.temperature?.toString())
    put("maxTokens", settings.maxTokens)
    put("topP", settings.topP?.toString())
    put("frequencyPenalty", settings.frequencyPenalty?.toString())
    put("presencePenalty", settings.presencePenalty?.toString())
    put("codeExecutionEnabled", settings.codeExecutionEnabled)
    put("googleSearchEnabled", settings.googleSearchEnabled)
    put("openAiWebSearchEnabled", settings.openAiWebSearchEnabled)
    put("thinkingEnabled", settings.thinkingEnabled)
    put("thinkingLevel", settings.thinkingLevel)
    put("thinkingBudgetEnabled", settings.thinkingBudgetEnabled)
    put("thinkingBudgetTokens", settings.thinkingBudgetTokens)
    put("openAiServiceTierEnabled", settings.openAiServiceTierEnabled)
    put("openAiServiceTier", settings.openAiServiceTier)
    put("webSearchEnabled", settings.webSearchEnabled)
    put("shellEnabled", settings.shellEnabled)
    put("lowContextModeEnabled", settings.lowContextModeEnabled)
}

private fun ApiKeyEntry.toJson(): JsonObject = buildJsonObject {
    put("id", id)
    put("name", name)
    put("key", key)
    put("provider", provider)
}

private fun settingsViewJson(settings: SettingsManager): JsonObject = buildJsonObject {
    put("systemPrompts", buildJsonArray {
        settings.systemPromptsSnapshot().forEach { add(promptJson(it)) }
    })
    put("embeddingModels", buildJsonArray {
        settings.embeddingModelsSnapshot().forEach { add(it.toJson()) }
    })
    put("activeEmbeddingModelId", settings.stringSnapshot("activeEmbeddingModelId"))
    put("fontPreference", settings.stringSnapshot("fontPreference"))
    put("customFontPath", settings.stringSnapshot("customFontPath"))
    put("customFontName", settings.stringSnapshot("customFontName"))
    put("activeSystemPromptId", settings.nullableStringSnapshot("activeSystemPromptId"))
    put("shellDevices", buildJsonArray {
        settings.shellDevicesSnapshot().forEach { add(it.toJson()) }
    })
    put("mcpServers", buildJsonArray {
        settings.mcpServersSnapshot().forEach { add(it.toJson()) }
    })
    put("customProviders", buildJsonArray {
        settings.customProvidersSnapshot().forEach { add(it.toJson()) }
    })
    put("conversationSettings", buildJsonObject {
        settings.conversationSettingsSnapshot().forEach { (id, value) ->
            put(id, conversationSettingsJson(value))
        }
    })
    put("apiKeys", buildJsonArray { settings.apiKeysSnapshot().forEach { add(it.toJson()) } })
    put("activeApiKeyIds", buildJsonObject {
        settings.activeApiKeyIdsSnapshot().forEach { (key, value) -> put(key, value) }
    })
    put("webSearchApiKeys", buildJsonObject {
        settings.webSearchApiKeysSnapshot().forEach { (key, value) -> put(key, value) }
    })
    put("proxyPassword", settings.proxyPasswordSnapshot())
}

// ---- memory / skill / files --------------------------------------------------------------------

private fun memoryJson(memory: MemoryManager): JsonObject = buildJsonObject {
    put("active", memory.getActiveMemory())
    put("meta", memory.getMetaJson())
    put("files", buildJsonArray {
        memory.listFiles().forEach { info ->
            add(buildJsonObject {
                put("name", info.name)
                put("description", info.description)
                put("content", memory.readFile(info.name))
            })
        }
    })
}

private fun skillsJson(skills: SkillManager): JsonObject = buildJsonObject {
    put("meta", skills.getMetaJson())
    put("files", buildJsonArray {
        skills.listFiles().forEach { info ->
            add(buildJsonObject {
                put("name", info.name)
                put("description", info.description)
                put("content", skills.readFile(info.name))
            })
        }
    })
}

private fun filesJson(filesDir: File): JsonArray = buildJsonArray {
    val rootPath = filesDir.toPath()
    filesDir.walkTopDown()
        .filter { it.isFile }
        .sortedBy { it.relativeTo(filesDir).invariantSeparatorsPath }
        .forEach { file ->
            add(buildJsonObject {
                put("name", rootPath.relativize(file.toPath()).toString().replace(File.separatorChar, '/'))
                put("base64", Base64.getEncoder().encodeToString(file.readBytes()))
            })
        }
}

// ---- table dump --------------------------------------------------------------------------------

private fun queryRows(
    connection: Connection,
    sql: String,
    render: (ResultSet) -> JsonElement,
): JsonArray = buildJsonArray {
    connection.createStatement().use { statement ->
        statement.executeQuery(sql).use { rows ->
            while (rows.next()) add(render(rows))
        }
    }
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, systemPromptId, " +
            "modelId, taskId, origin, graduated, draftText, draftAttachments, " +
            "selectedRunBranchesJson, hasUnreadGeneration, isPinned FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("title", row.getString(2))
            put("lastUpdated", row.getLong(3))
            put("dataChangedAt", row.getLong(4))
            put("selectedBranchesJson", row.getString(5))
            put("systemPromptId", row.getString(6))
            put("modelId", row.getString(7))
            put("taskId", row.getString(8))
            put("origin", row.getString(9))
            put("graduated", row.getInt(10) != 0)
            put("draftText", row.getString(11))
            put("draftAttachments", row.getString(12))
            put("selectedRunBranchesJson", row.getString(13))
            put("hasUnreadGeneration", row.getInt(14) != 0)
            put("isPinned", row.getInt(15) != 0)
        }
    })
    put("runs", queryRows(
        connection,
        "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, " +
            "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous " +
            "FROM runs ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentRunId", row.getString(3))
            put("status", row.getString(4))
            put("activeSlot", (row.getObject(5) as? Number)?.toInt())
            put("startedAt", row.getLong(6))
            put("lastCheckpointAt", row.getLong(7))
            put("stopRequestedAt", (row.getObject(8) as? Number)?.toLong())
            put("endedAt", (row.getObject(9) as? Number)?.toLong())
            put("endReason", row.getString(10))
            put("currentPass", row.getInt(11))
            put("legacyAmbiguous", row.getInt(12) != 0)
        }
    })
    put("messages", queryRows(
        connection,
        "SELECT id, conversationId, parentId, text, images, tokenCount, status, participant, " +
            "timestamp, modelName, attachmentMeta, runId, runSequence, consumedAtPass, " +
            "sourceJson, toolCallJson FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentId", row.getString(3))
            put("text", row.getString(4))
            put("images", row.getString(5))
            put("tokenCount", row.getInt(6))
            put("status", row.getString(7))
            put("participant", row.getString(8))
            put("timestamp", row.getLong(9))
            put("modelName", row.getString(10))
            put("attachmentMeta", row.getString(11))
            put("runId", row.getString(12))
            put("runSequence", row.getLong(13))
            put("consumedAtPass", (row.getObject(14) as? Number)?.toInt())
            put("sourceJson", row.getString(15))
            put("toolCallJson", row.getString(16))
        }
    })
    put("tasks", queryRows(
        connection,
        "SELECT id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, runAt, " +
            "nextRunAt, enabled, createdAt, lastRunAt FROM tasks ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("name", row.getString(2))
            put("prompt", row.getString(3))
            put("systemPrompt", row.getString(4))
            put("systemPromptId", row.getString(5))
            put("modelId", row.getString(6))
            put("cronExpr", row.getString(7))
            put("runAt", (row.getObject(8) as? Number)?.toLong())
            put("nextRunAt", row.getLong(9))
            put("enabled", row.getInt(10) != 0)
            put("createdAt", row.getLong(11))
            put("lastRunAt", (row.getObject(12) as? Number)?.toLong())
        }
    })
    put("loops", queryRows(
        connection,
        "SELECT conversationId, intervalMs, prompt, nextFireAt, cycleCount, maxCycles, active, " +
            "revision FROM loops ORDER BY conversationId",
    ) { row ->
        buildJsonObject {
            put("conversationId", row.getString(1))
            put("intervalMs", row.getLong(2))
            put("prompt", row.getString(3))
            put("nextFireAt", row.getLong(4))
            put("cycleCount", row.getInt(5))
            put("maxCycles", (row.getObject(6) as? Number)?.toInt())
            put("active", row.getInt(7) != 0)
            put("revision", row.getLong(8))
        }
    })
    put("embeddings", queryRows(
        connection,
        "SELECT id, messageId, modelId, chunkText, dimension FROM embeddings ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getLong(1))
            put("messageId", row.getString(2))
            put("modelId", row.getString(3))
            put("chunkText", row.getString(4))
            put("dimension", row.getInt(5))
        }
    })
    put("conversation_settings_transfer", queryRows(
        connection,
        "SELECT conversationId, settingsJson FROM conversation_settings_transfer ORDER BY conversationId",
    ) { row ->
        buildJsonObject {
            put("conversationId", row.getString(1))
            put("settingsJson", row.getString(2))
        }
    })
    put("conversation_settings_import_transfer", queryRows(
        connection,
        "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getInt(1))
            put("transferId", row.getString(2))
            put("settingsJson", row.getString(3))
            put("mode", row.getString(4))
        }
    })
    put("maintenance_debt", queryRows(
        connection,
        "SELECT kind, identity, state, revision, updatedAt FROM maintenance_debt ORDER BY kind, identity",
    ) { row ->
        buildJsonObject {
            put("kind", row.getString(1))
            put("identity", row.getString(2))
            put("state", row.getString(3))
            put("revision", row.getInt(4))
            put("updatedAt", row.getLong(5))
        }
    })
    put("semantic_index_ledger", queryRows(
        connection,
        "SELECT modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt " +
            "FROM semantic_index_ledger ORDER BY modelId",
    ) { row ->
        buildJsonObject {
            put("modelId", row.getString(1))
            put("state", row.getString(2))
            put("sourceRevision", row.getInt(3))
            put("completedRevision", row.getInt(4))
            put("reconcileRevision", row.getInt(5))
            put("updatedAt", row.getLong(6))
        }
    })
}

// ---- scenario runner ---------------------------------------------------------------------------

private fun executeScript(connection: Connection, text: String) {
    val statements = text.lines()
        .map { line -> line.substringBefore("--") }
        .joinToString("\n")
        .split(";")
    for (statement in statements) {
        if (statement.isBlank()) continue
        connection.createStatement().use { it.execute(statement) }
    }
}

private fun maskTrace(text: String, root: File): String {
    val rooted = text.replace(root.absolutePath, "<root>")
    val ordinals = LinkedHashMap<String, String>()
    val maskedUuids = UUID_PATTERN.replace(rooted) { match ->
        ordinals.getOrPut(match.value) { "<uuid:${ordinals.size}>" }
    }
    return EPOCH_MILLIS.replace(maskedUuids, "\"<clock>\"")
}

private fun decisionsOf(step: JsonObject): Map<DataExporter.ExportCategory, DataImporter.ImportStrategy> {
    val root = step["decisions"] as? JsonObject ?: return emptyMap()
    val out = linkedMapOf<DataExporter.ExportCategory, DataImporter.ImportStrategy>()
    root.forEach { (key, value) ->
        out[DataExporter.ExportCategory.valueOf(key)] =
            DataImporter.ImportStrategy.valueOf((value as JsonPrimitive).content)
    }
    return out
}

private fun importResultJson(result: DataImporter.ImportResult): JsonObject = buildJsonObject {
    put("conversationsImported", result.conversationsImported)
    put("tasksImported", result.tasksImported)
    put("loopsImported", result.loopsImported)
    put("memoriesImported", result.memoriesImported)
    put("systemPromptsImported", result.systemPromptsImported)
    put("settingsImported", result.settingsImported)
    put("apiKeysImported", result.apiKeysImported)
    put("errors", buildJsonArray { result.errors.forEach { add(it) } })
}

private fun previewJson(preview: DataImporter.ImportPreview): JsonObject = buildJsonObject {
    put("manifest", buildJsonObject {
        put("version", preview.manifest.version)
        put("appVersion", preview.manifest.appVersion)
        put("exportedAt", preview.manifest.exportedAt)
        put("categories", buildJsonArray { preview.manifest.categories.forEach { add(it) } })
        put("hasApiKeys", preview.manifest.hasApiKeys)
    })
    put("conversationCount", preview.conversationCount)
    put("taskCount", preview.taskCount)
    put("memoryCount", preview.memoryCount)
    put("systemPromptCount", preview.systemPromptCount)
    put("settingsPresent", preview.settingsPresent)
    put("apiKeysPresent", preview.apiKeysPresent)
    put("hasConversationGraph", preview.hasConversationGraph)
    put("hasImportableData", preview.hasImportableData)
    put("isSupportedVersion", preview.isSupportedVersion)
}

private fun render(scenarioFile: File): String = runBlocking {
    val root = Files.createTempDirectory("agora-data-importer-").toFile()
    try {
        val scenario = scenarioJson.parseToJsonElement(scenarioFile.readText()).jsonObject
        val databaseFile = File(root, "db/agora_db").also { it.parentFile.mkdirs() }
        val filesDir = File(root, "files").also { it.mkdirs() }
        val cacheDir = File(root, "cache").also { it.mkdirs() }
        val archivesDir = File(root, "archives").also { it.mkdirs() }

        DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
            connection.createStatement().use { it.execute("PRAGMA foreign_keys = ON") }
            executeScript(connection, File("platform/schema.sql").readText())
            for (statement in scenario.stringList("seedSql")) {
                connection.createStatement().use { it.execute(statement) }
            }
        }

        val conversationSettings: Map<String, ConversationSettings> =
            (scenario["conversationSettings"] as? JsonObject)?.mapValues { (_, element) ->
                json.decodeFromJsonElement<ConversationSettings>(element)
            } ?: emptyMap()
        val systemPrompts: List<SystemPromptEntry> =
            (scenario["systemPrompts"] as? JsonArray)?.map {
                json.decodeFromJsonElement<SystemPromptEntry>(it)
            } ?: emptyList()
        val apiKeys: List<ApiKeyEntry> =
            (scenario["apiKeys"] as? JsonArray)?.map {
                json.decodeFromJsonElement<ApiKeyEntry>(it)
            } ?: emptyList()
        val activeApiKeyIds: Map<String, String> =
            (scenario["activeApiKeyIds"] as? JsonObject)?.mapValues {
                (it.value as JsonPrimitive).content
            } ?: emptyMap()
        val webSearchApiKeys: Map<String, String> =
            (scenario["webSearchApiKeys"] as? JsonObject)?.mapValues {
                (it.value as JsonPrimitive).content
            } ?: emptyMap()
        val proxyPassword = scenario.stringOrNull("proxyPassword") ?: ""

        val settingsManager = buildSettings(
            scenario["settings"] as? JsonObject ?: error("scenario settings missing"),
            conversationSettings,
            systemPrompts,
            apiKeys,
            activeApiKeyIds,
            webSearchApiKeys,
            proxyPassword,
        )
        val memoryManager = MemoryManager(Context(
            databaseFile = databaseFile,
            filesDir = filesDir,
            cacheDir = cacheDir,
            packageName = "com.newoether.agora",
            packageManager = PackageManager("9.9.9"),
            contentResolver = ContentResolver(),
        ))
        val skillManager = SkillManager(Context(
            databaseFile = databaseFile,
            filesDir = filesDir,
            cacheDir = cacheDir,
            packageName = "com.newoether.agora",
            packageManager = PackageManager("9.9.9"),
            contentResolver = ContentResolver(),
        ))
        (scenario["memory"] as? JsonObject)?.let { memory ->
            memory.stringOrNull("active")?.let { memoryManager.updateActiveMemory(it, "seed") }
            (memory["files"] as? JsonObject)?.forEach { (name, content) ->
                memoryManager.createFile(name, (content as JsonPrimitive).content)
            }
            memory.stringOrNull("meta")?.let { memoryManager.saveMetaJson(it) }
        }
        (scenario["skills"] as? JsonObject)?.let { skills ->
            (skills["files"] as? JsonObject)?.forEach { (name, content) ->
                skillManager.createFile(name, (content as JsonPrimitive).content)
            }
            skills.stringOrNull("meta")?.let { skillManager.saveMetaJson(it) }
        }

        val archiveFiles = mutableMapOf<String, File>()
        val context = Context(
            databaseFile = databaseFile,
            filesDir = filesDir,
            cacheDir = cacheDir,
            packageName = "com.newoether.agora",
            packageManager = PackageManager(scenario.stringOrNull("appVersion") ?: "9.9.9"),
            contentResolver = ContentResolver { uri ->
                archiveFiles[uri.toString()]?.let { ParcelFileDescriptor(it) }
            },
        )
        val database = ChatDatabase.build(context)
        val settingsRepository = SettingsRepository(
            settingsManager = settingsManager,
            failInitialLoad = scenario.stringOrNull("failInitialLoad"),
            failImportApply = scenario.stringOrNull("failImportApply"),
        )
        val coordinator = ConversationSettingsTransferCoordinator(
            conversations = ConversationRepository(database.chatDao()),
            settings = settingsRepository,
        )
        val importer = DataImporter(
            context = context,
            database = database,
            chatDao = database.chatDao(),
            settingsManager = settingsManager,
            memoryManager = memoryManager,
            skillManager = skillManager,
            conversationSettingsTransfers = coordinator,
        )

        val steps = mutableListOf<JsonElement>()
        var stepIndex = 0
        for (element in scenario.stringJsonArray("steps")) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                when (op) {
                    "preview", "import" -> {
                        val entries = (step["archive"] as? JsonArray)
                            ?: (scenario["archive"] as? JsonArray)
                            ?: error("step $stepIndex has no archive")
                        val zipFile = File(archivesDir, "archive-$stepIndex.zip")
                        writeArchive(entries, zipFile)
                        val uri = Uri.parse("content://stand/archive-$stepIndex.zip")
                        archiveFiles[uri.toString()] = zipFile
                        when (op) {
                            "preview" -> previewJson(importer.preview(uri))
                            else -> {
                                val progress = mutableListOf<Float>()
                                val importResult = importer.import(uri, decisionsOf(step)) {
                                    progress += it
                                }
                                buildJsonObject {
                                    put("result", importResultJson(importResult))
                                    put("progress", buildJsonArray { progress.forEach { add(it.toString()) } })
                                }
                            }
                        }
                    }
                    "font" -> {
                        val bytes = Base64.getDecoder().decode(
                            (step["base64"] as? JsonPrimitive)?.content
                                ?: error("font step needs base64"),
                        )
                        val file = File(root, "font-$stepIndex.ttf")
                        file.writeBytes(bytes)
                        val meta = readFontMeta(file)
                        buildJsonObject {
                            put("family", meta.family)
                            put("subfamily", meta.subfamily)
                        }
                    }
                    else -> error("unknown step op $op")
                }
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", buildJsonObject {
                        put("type", failure::class.simpleName ?: failure::class.java.name)
                        put("message", failure.message ?: "")
                    })
                }
            }
            stepIndex++

            val stepJson = buildJsonObject {
                put("op", op)
                put("result", result)
                if (op == "import") {
                    put("settingsCalls", buildJsonArray {
                        settingsManager.calls
                            .filter { (it["name"] as JsonPrimitive).content in TRACED_SETTINGS_CALLS }
                            .forEach { add(it) }
                    })
                    put("settingsView", settingsViewJson(settingsManager))
                    put("memory", memoryJson(memoryManager))
                    put("skills", skillsJson(skillManager))
                    put("files", filesJson(filesDir))
                    DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
                        put("tables", dumpTables(connection))
                    }
                    put("appliedImports", buildJsonArray {
                        settingsRepository.appliedImports.forEach { (replace, imported) ->
                            add(buildJsonObject {
                                put("replace", replace)
                                put("settings", buildJsonObject {
                                    imported.forEach { (id, value) -> put(id, conversationSettingsJson(value)) }
                                })
                            })
                        }
                    })
                }
            }
            steps += stepJson
        }

        val trace = buildJsonObject {
            put("steps", buildJsonArray { steps.forEach { add(it) } })
        }.toString()
        maskTrace(trace, root)
    } finally {
        root.deleteRecursively()
    }
}
