// Golden trace driver for data/DataExporter.kt.
//
// DataExporter.kt, ConversationExportSnapshotReader.kt, ExportSpool.kt, NativeBackupV5Writer.kt,
// NativeBackupV5Baseline.kt, NativeBackupMediaPolicy.kt, NativeBackupSecretsPolicy.kt,
// PortableSettingsArchive.kt, MemoryManager.kt, SkillManager.kt and every model they reference
// compile VERBATIM from the app tree. The only substitutes are the Android platform surface
// (Context/cache/files dirs, ContentResolver, PackageManager, Uri), the DataStore SettingsManager
// double, and Apache Commons Compress (real jar).
//
// Each `export` step reports: the progress floats, the ExportResult or the thrown error, the
// archive's entry names/contents, and whether the temporary spool was cleaned up. Media entry
// names carry UUID.randomUUID() entropy; the driver masks exactly the UUIDs that appear in
// `media/` entry names, in first-appearance order, and masks the manifest's exported_at instant.
package data.exporter.golden

import android.content.Context
import android.content.ContentResolver
import android.content.pm.PackageManager
import android.net.Uri
import com.newoether.agora.data.ApiKeyEntry
import com.newoether.agora.data.ConversationSettings
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.DataExporter
import com.newoether.agora.data.EmbeddingModelConfig
import com.newoether.agora.data.McpServerConfig
import com.newoether.agora.data.SettingsManager
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.data.SystemPromptEntry
import java.io.File
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import java.util.Base64
import java.util.zip.ZipEntry
import java.util.zip.ZipFile
import java.util.zip.ZipOutputStream
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.decodeFromString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

private val json = Json { ignoreUnknownKeys = true }
private val scenarioJson = Json { ignoreUnknownKeys = true }
private val UUID_PATTERN =
    Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
private val EXPORTED_AT_PATTERN = Regex("\"exported_at\":\"[^\"]*\"")

private fun required(root: JsonObject, key: String): JsonElement =
    root[key] ?: error("settings snapshot is missing `$key`")

private inline fun <reified T> value(root: JsonObject, key: String): T =
    json.decodeFromJsonElement(required(root, key))

private inline fun <reified T> nullableValue(root: JsonObject, key: String): T? {
    val element = required(root, key)
    if (element === JsonNull) return null
    return json.decodeFromJsonElement<T>(element)
}

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

private fun render(scenarioFile: File): String = runBlocking {
    val root = Files.createTempDirectory("agora-export-").toFile()
    try {
        val mediaRoot = File(root, "media").also { it.mkdirs() }
        val filesDir = File(root, "files").also { it.mkdirs() }
        val cacheDir = File(root, "cache").also { it.mkdirs() }
        val outputDir = File(root, "out").also { it.mkdirs() }
        val databaseFile = File(root, "db/agora_db").also { it.parentFile.mkdirs() }

        val fontSpec = scenarioJson.parseToJsonElement(scenarioFile.readText()).jsonObject["font"]
            ?.jsonObject
        val fontFile = fontSpec?.get("name")?.jsonPrimitive?.contentOrNull
            ?.let { File(filesDir, "font/$it") }
        val substituted = scenarioFile.readText()
            .replace("<root>", root.path)
            .replace("<media>", mediaRoot.path)
            .replace("<font>", fontFile?.path ?: File(filesDir, "font/missing").path)
        val scenario = scenarioJson.parseToJsonElement(substituted).jsonObject

        if (fontSpec != null && fontSpec["missing"]?.jsonPrimitive?.booleanOrNull != true) {
            fontFile!!.parentFile.mkdirs()
            fontFile.writeBytes(
                Base64.getDecoder().decode(
                    fontSpec["base64"]?.jsonPrimitive?.contentOrNull.orEmpty(),
                ),
            )
        }

        val resolver = ContentResolver()
        for (element in scenario["media"] as? JsonArray ?: JsonArray(emptyList())) {
            val entry = element.jsonObject
            val relative = entry["file"]?.jsonPrimitive?.contentOrNull
            val bytes = entry["base64"]?.jsonPrimitive?.contentOrNull
            val file = relative?.let { File(mediaRoot, it) }
            if (file != null && bytes != null) {
                file.parentFile.mkdirs()
                file.writeBytes(Base64.getDecoder().decode(bytes))
            }
            val uri = entry["uri"]?.jsonPrimitive?.contentOrNull
            if (uri != null && file != null) resolver.inputs[uri] = file
        }

        writeCatalog(filesDir, "memory_db", "memory_meta.json", scenario["memory"]?.jsonObject)
        File(filesDir, "active_memory.md").apply {
            scenario["memory"]?.jsonObject?.get("active")?.jsonPrimitive?.contentOrNull
                ?.let { writeText(it) }
        }
        writeCatalog(filesDir, "skill_db", "skill_meta.json", scenario["skills"]?.jsonObject)

        DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
            connection.createStatement().use { it.execute("PRAGMA foreign_keys = ON") }
            executeScript(connection, File("platform/schema.sql").readText())
            for (element in scenario["seedSql"] as? JsonArray ?: JsonArray(emptyList())) {
                connection.createStatement().use { it.execute(element.jsonPrimitive.content) }
            }
        }

        val settingsRoot = scenario["settings"]?.jsonObject
            ?: error("scenario is missing `settings`")
        val conversationSettings =
            scenario["conversationSettings"]?.let {
                json.decodeFromJsonElement<Map<String, ConversationSettings>>(it)
            } ?: emptyMap()
        val systemPrompts =
            scenario["systemPrompts"]?.let {
                json.decodeFromJsonElement<List<SystemPromptEntry>>(it)
            } ?: emptyList()
        val apiKeys = scenario["apiKeys"]?.let {
            json.decodeFromJsonElement<List<ApiKeyEntry>>(it)
        } ?: emptyList()
        val activeApiKeyIds = scenario["activeApiKeyIds"]?.let {
            json.decodeFromJsonElement<Map<String, String>>(it)
        } ?: emptyMap()
        val webSearchApiKeys = scenario["webSearchApiKeys"]?.let {
            json.decodeFromJsonElement<Map<String, String>>(it)
        } ?: emptyMap()
        val proxyPassword = scenario["proxyPassword"]?.jsonPrimitive?.contentOrNull ?: ""

        val settings = buildSettings(
            settingsRoot,
            conversationSettings,
            systemPrompts,
            apiKeys,
            activeApiKeyIds,
            webSearchApiKeys,
            proxyPassword,
        )
        val context = Context(
            databaseFile = databaseFile,
            filesDir = filesDir,
            cacheDir = cacheDir,
            packageName = "com.newoether.agora",
            packageManager = PackageManager(
                scenario["appVersion"]?.jsonPrimitive?.contentOrNull,
            ),
            contentResolver = resolver,
        )
        val exporter = DataExporter(
            context = context,
            settingsManager = settings,
            memoryManager = com.newoether.agora.data.MemoryManager(context),
            skillManager = com.newoether.agora.data.SkillManager(context),
        )

        val outputs = mutableListOf<File>()
        val steps = mutableListOf<JsonElement>()
        for ((index, element) in (scenario["steps"] as JsonArray).withIndex()) {
            val step = element.jsonObject
            val categories = (step["categories"]?.jsonArray ?: JsonArray(emptyList()))
                .map { DataExporter.ExportCategory.fromManifestKey(it.jsonPrimitive.content)!! }
                .toSet()
            val includeApiKeys = step["includeApiKeys"]?.jsonPrimitive?.booleanOrNull ?: false
            val baselineStep = step["baselineStep"]?.jsonPrimitive?.intOrNull
            val baselineFile = baselineStep?.let { outputs[it] }
            if (step["corruptBaseline"]?.jsonPrimitive?.booleanOrNull == true) {
                baselineFile?.writeText("not a zip")
            }
            if (step["stampBaseline"]?.jsonPrimitive?.booleanOrNull == true) {
                baselineFile?.let(::stampBaseline)
            }
            for (element in step["preSql"] as? JsonArray ?: JsonArray(emptyList())) {
                DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
                    connection.createStatement().use { it.execute(element.jsonPrimitive.content) }
                }
            }
            val outputFile = if (step["output"]?.jsonPrimitive?.contentOrNull == "unavailable") {
                null
            } else {
                File(outputDir, "step-$index.zip").also { outputs += it }
            }
            val outputUri = if (outputFile == null) {
                Uri.parse("content://missing/destination")
            } else {
                Uri.parse("file://${outputFile.path}").also {
                    resolver.outputs[it.toString()] = outputFile
                }
            }

            val progress = mutableListOf<String>()
            val result = try {
                val exported = exporter.export(
                    uri = outputUri,
                    categories = categories,
                    includeApiKeys = includeApiKeys,
                    baselineFile = baselineFile,
                    onProgress = { progress += it.toString() },
                )
                buildJsonObject {
                    put("imagesExported", exported.imagesExported)
                    put("missingResourceCount", exported.missingResourceCount)
                }
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", buildJsonObject {
                        put("type", failure::class.simpleName ?: failure::class.java.name)
                        put("message", failure.message ?: "")
                    })
                }
            }

            val archive = if (outputFile?.isFile == true) archiveEntries(outputFile) else JsonArray(emptyList())
            steps.add(buildJsonObject {
                put("op", "export")
                put("progress", buildJsonArray { progress.forEach { add(it) } })
                put("result", result)
                put("archive", archive)
                put("cacheFileCount", cacheDir.listFiles()?.size ?: 0)
            })
        }

        val trace = buildJsonObject {
            put("steps", JsonArray(steps))
        }
        scenarioJson.encodeToString(JsonObject.serializer(), trace)
    } finally {
        root.deleteRecursively()
    }
}

private fun writeCatalog(filesDir: File, name: String, metaName: String, spec: JsonObject?) {
    if (spec == null) return
    val dir = File(filesDir, name).also { it.mkdirs() }
    spec["files"]?.jsonObject?.forEach { (fileName, content) ->
        val file = File(dir, fileName)
        file.parentFile.mkdirs()
        file.writeText(content.jsonPrimitive.content)
    }
    spec["meta"]?.jsonPrimitive?.contentOrNull?.let { File(dir, metaName).writeText(it) }
}

private fun archiveEntries(file: File): JsonArray {
    val generated = mutableSetOf<String>()
    ZipFile(file).use { zip ->
        for (entry in zip.entries()) {
            if (entry.name.startsWith("media/")) {
                UUID_PATTERN.find(entry.name)?.let { generated += it.value }
            }
        }
    }
    val ordinals = mutableMapOf<String, Int>()
    fun mask(text: String): String = UUID_PATTERN.replace(text) { match ->
        if (match.value in generated) {
            "<uuid:${ordinals.getOrPut(match.value) { ordinals.size }}>"
        } else {
            match.value
        }
    }
    return buildJsonArray {
        ZipFile(file).use { zip ->
            for (entry in zip.entries()) {
                val name = mask(entry.name)
                val bytes = zip.getInputStream(entry).use { it.readBytes() }
                add(buildJsonObject {
                    put("name", name)
                    if (entry.name.startsWith("media/") || entry.name.startsWith("custom_font/")) {
                        put("base64", Base64.getEncoder().encodeToString(bytes))
                    } else {
                        val text = mask(String(bytes, Charsets.UTF_8))
                            .replace(EXPORTED_AT_PATTERN, "\"exported_at\":\"<clock>\"")
                        put(
                            "text",
                            if (entry.name == "conv/index.json") canonicalizeIndex(text) else text,
                        )
                    }
                })
            }
        }
    }
}

private val UUID_ORDINAL = Regex("<uuid:(\\d+)>")

/**
 * The index writer sorts media entries lexicographically by their UUID names. UUID entropy is
 * platform-generated, so the driver re-sorts each masked array by first-appearance ordinal; the
 * pinned property is the sorted set, not the random order the entropy happened to produce.
 */
private fun canonicalizeIndex(text: String): String {
    val root = scenarioJson.parseToJsonElement(text).jsonObject
    val conversations = (root["conversations"] as JsonArray).map { element ->
        val item = element.jsonObject
        val media = (item["mediaEntries"] as? JsonArray)
            ?.map { it.jsonPrimitive.content }
            ?.sortedBy { entry ->
                UUID_ORDINAL.find(entry)?.groupValues?.get(1)?.toIntOrNull() ?: 0
            }
        buildJsonObject {
            item.forEach { (key, value) -> if (key != "mediaEntries") put(key, value) }
            if (media != null) {
                put("mediaEntries", buildJsonArray { media.forEach { add(it) } })
            }
        }
    }
    return buildJsonObject {
        root.forEach { (key, value) -> if (key != "conversations") put(key, value) }
        put("conversations", buildJsonArray { conversations.forEach { add(it) } })
    }.toString()
}

private fun stampBaseline(file: File) {
    val entries = mutableListOf<Pair<String, ByteArray>>()
    ZipFile(file).use { zip ->
        for (entry in zip.entries()) {
            entries += entry.name to zip.getInputStream(entry).use { it.readBytes() }
        }
    }
    val stamped = File(file.parentFile, file.name + ".stamped")
    ZipOutputStream(stamped.outputStream()).use { out ->
        for ((name, bytes) in entries) {
            val content = if (name == "manifest.json") {
                val text = String(bytes, Charsets.UTF_8)
                require(text.endsWith("}"))
                (text.dropLast(1) + ",\"incremental_baseline\":1}").toByteArray()
            } else {
                bytes
            }
            out.putNextEntry(ZipEntry(name))
            out.write(content)
            out.closeEntry()
        }
        out.finish()
    }
    check(file.delete())
    check(stamped.renameTo(file))
}

private fun executeScript(connection: Connection, text: String) {    val statements = text.lines()
        .map { line -> line.substringBefore("--") }
        .joinToString("\n")
        .split(";")
    for (statement in statements) {
        if (statement.isBlank()) continue
        connection.createStatement().use { it.execute(statement) }
    }
}
