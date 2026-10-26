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

internal val json = Json { ignoreUnknownKeys = true }
internal val scenarioJson = Json { ignoreUnknownKeys = true }
internal val UUID_PATTERN =
    Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")
internal val EPOCH_MILLIS = Regex("(?<![0-9.])1[7-9][0-9]{11}(?![0-9])")

internal val TRACED_SETTINGS_CALLS = setOf(
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

internal fun required(root: JsonObject, key: String): JsonElement =
    root[key] ?: error("settings snapshot is missing `$key`")

internal inline fun <reified T> value(root: JsonObject, key: String): T =
    json.decodeFromJsonElement(required(root, key))

internal inline fun <reified T> nullableValue(root: JsonObject, key: String): T? {
    val element = required(root, key)
    if (element === JsonNull) return null
    return json.decodeFromJsonElement<T>(element)
}

internal fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

internal fun JsonObject.stringOrNull(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

internal fun JsonObject.stringList(name: String): List<String> =
    (this[name] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()

internal fun JsonObject.stringJsonArray(name: String): JsonArray =
    this[name] as? JsonArray ?: JsonArray(emptyList())

// ---- scenario runner ---------------------------------------------------------------------------

internal fun executeScript(connection: Connection, text: String) {
    val statements = text.lines()
        .map { line -> line.substringBefore("--") }
        .joinToString("\n")
        .split(";")
    for (statement in statements) {
        if (statement.isBlank()) continue
        connection.createStatement().use { it.execute(statement) }
    }
}

internal fun maskTrace(text: String, root: File): String {
    val rooted = text.replace(root.absolutePath, "<root>")
    val ordinals = LinkedHashMap<String, String>()
    val maskedUuids = UUID_PATTERN.replace(rooted) { match ->
        ordinals.getOrPut(match.value) { "<uuid:${ordinals.size}>" }
    }
    return EPOCH_MILLIS.replace(maskedUuids, "\"<clock>\"")
}

internal fun decisionsOf(step: JsonObject): Map<DataExporter.ExportCategory, DataImporter.ImportStrategy> {
    val root = step["decisions"] as? JsonObject ?: return emptyMap()
    val out = linkedMapOf<DataExporter.ExportCategory, DataImporter.ImportStrategy>()
    root.forEach { (key, value) ->
        out[DataExporter.ExportCategory.valueOf(key)] =
            DataImporter.ImportStrategy.valueOf((value as JsonPrimitive).content)
    }
    return out
}

internal fun importResultJson(result: DataImporter.ImportResult): JsonObject = buildJsonObject {
    put("conversationsImported", result.conversationsImported)
    put("tasksImported", result.tasksImported)
    put("loopsImported", result.loopsImported)
    put("memoriesImported", result.memoriesImported)
    put("systemPromptsImported", result.systemPromptsImported)
    put("settingsImported", result.settingsImported)
    put("apiKeysImported", result.apiKeysImported)
    put("errors", buildJsonArray { result.errors.forEach { add(it) } })
}

internal fun previewJson(preview: DataImporter.ImportPreview): JsonObject = buildJsonObject {
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

internal fun render(scenarioFile: File): String = runBlocking {
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
