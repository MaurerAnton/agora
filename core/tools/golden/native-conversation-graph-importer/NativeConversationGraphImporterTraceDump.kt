// Golden trace driver for data/NativeConversationGraphImporter.kt.
//
// The importer and every module it touches compile VERBATIM from the app tree (see run_golden.sh
// for the full list and the three substituted boundaries: vendored AOSP android.util.Json*,
// the DataImporter pieces lifted by this stand's extractor, and the Android/Room surface).
//
// Each scenario runs any of the importer's three public phases in order against one shared
// stand database:
//   count    -> countConversationGraph over a chained v5 (or legacy) graph stream
//   headers  -> readConversationGraphHeaders: sanitize + resolve + terminalize/parent-order +
//               loop sanitize + conversation-settings extraction, with the strategy's MERGE
//               id-set queries against the seeded database
//   import   -> importConversationGraph: the real transaction, writes, settings transfer,
//               maintenance enqueues and semantic mutation — traced through the resulting tables
//
// Determinism: the transferId UUID and the System.currentTimeMillis() clock are masked after
// serialization (first-appearance <uuid:N> ordinals; the epoch value becomes "<clock>"). Fixture
// rows never carry epoch-millisecond or UUID-shaped values, so the substitutions are unambiguous.
// The importJson config, the resolveSystemPromptId lambda and scheduleMaintenance are injected
// exactly like the real call site (Json { ignoreUnknownKeys = true; coerceInputValues = true },
// scenario map lookup with containsKey semantics, recording lambda).
package native.conversation.graph.importer.golden

import android.content.ContentResolver
import android.content.Context
import android.content.pm.PackageManager
import com.newoether.agora.data.DataImporter
import com.newoether.agora.data.NativeConversationGraphImporter
import com.newoether.agora.data.NativeConversationGraphSource
import com.newoether.agora.data.NativeConversationMediaRestorer
import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.TaskEntity
import com.newoether.agora.data.NativeGraphEntrySource
import com.newoether.agora.data.ConversationSettings
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import java.sql.ResultSet
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
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val scenarioJson = Json { ignoreUnknownKeys = true }

private val UUID_PATTERN =
    Regex("[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}")

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
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

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.stringOrNull(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.boolean(name: String, fallback: Boolean): Boolean {
    val value = this[name] as? JsonPrimitive ?: return fallback
    return value.content.toBooleanStrict()
}

private fun JsonObject.stringList(name: String): List<String> =
    (this[name] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList()

// ---- the entry-source double (same contract as the graph-source stand) --------------------------

private class MemoryStream(private val bytes: ByteArray) : InputStream() {
    private var pos = 0
    private var closed = false
    override fun read(): Int {
        if (closed) throw IOExceptionClosed
        if (pos >= bytes.size) return -1
        return bytes[pos++].toInt() and 0xFF
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        if (closed) throw IOExceptionClosed
        if (pos >= bytes.size) return -1
        val take = minOf(len, bytes.size - pos)
        System.arraycopy(bytes, pos, b, off, take)
        pos += take
        return take
    }

    override fun close() {
        closed = true
    }

    private object IOExceptionClosed : java.io.IOException("Stream Closed")
}

private class MapEntrySource(entries: Map<String, String>) : NativeGraphEntrySource {
    private val data = entries.mapValues { it.value.toByteArray(Charsets.UTF_8) }

    override fun has(name: String): Boolean = data.containsKey(name)

    override fun bytes(name: String): ByteArray? = data[name]

    override fun stream(name: String): InputStream? = data[name]?.let { MemoryStream(it) }
}

// ---- entity dumps ------------------------------------------------------------------------------

private fun conversationJson(entity: ChatEntity): JsonObject = buildJsonObject {
    put("id", entity.id)
    put("title", entity.title)
    put("lastUpdated", entity.lastUpdated)
    put("dataChangedAt", entity.dataChangedAt)
    put("selectedBranchesJson", entity.selectedBranchesJson)
    put("systemPromptId", entity.systemPromptId)
    put("modelId", entity.modelId)
    put("taskId", entity.taskId)
    put("origin", entity.origin)
    put("graduated", entity.graduated)
    put("draftText", entity.draftText)
    put("draftAttachments", entity.draftAttachments)
    put("selectedRunBranchesJson", entity.selectedRunBranchesJson)
    put("hasUnreadGeneration", entity.hasUnreadGeneration)
    put("isPinned", entity.isPinned)
}

private fun runJson(entity: RunEntity): JsonObject = buildJsonObject {
    put("id", entity.id)
    put("conversationId", entity.conversationId)
    put("parentRunId", entity.parentRunId)
    put("status", entity.status.name)
    put("activeSlot", entity.activeSlot)
    put("startedAt", entity.startedAt)
    put("lastCheckpointAt", entity.lastCheckpointAt)
    put("stopRequestedAt", entity.stopRequestedAt)
    put("endedAt", entity.endedAt)
    put("endReason", entity.endReason?.name)
    put("currentPass", entity.currentPass)
    put("legacyAmbiguous", entity.legacyAmbiguous)
}

private fun taskJson(entity: TaskEntity): JsonObject = buildJsonObject {
    put("id", entity.id)
    put("name", entity.name)
    put("prompt", entity.prompt)
    put("systemPrompt", entity.systemPrompt)
    put("systemPromptId", entity.systemPromptId)
    put("modelId", entity.modelId)
    put("cronExpr", entity.cronExpr)
    put("runAt", entity.runAt)
    put("nextRunAt", entity.nextRunAt)
    put("enabled", entity.enabled)
    put("createdAt", entity.createdAt)
    put("lastRunAt", entity.lastRunAt)
}

private fun loopJson(entity: LoopEntity): JsonObject = buildJsonObject {
    put("conversationId", entity.conversationId)
    put("intervalMs", entity.intervalMs)
    put("prompt", entity.prompt)
    put("nextFireAt", entity.nextFireAt)
    put("cycleCount", entity.cycleCount)
    put("maxCycles", entity.maxCycles)
    put("active", entity.active)
    put("revision", entity.revision)
}

/** Only non-null fields, mirroring kotlinx' encodeDefaults = false over all-default nulls. */
private fun conversationSettingsJson(settings: ConversationSettings): JsonObject = buildJsonObject {
    settings.contextWindow?.let { put("contextWindow", it) }
    settings.temperature?.let { put("temperature", it.toString()) }
    settings.maxTokens?.let { put("maxTokens", it) }
    settings.topP?.let { put("topP", it.toString()) }
    settings.frequencyPenalty?.let { put("frequencyPenalty", it.toString()) }
    settings.presencePenalty?.let { put("presencePenalty", it.toString()) }
    settings.codeExecutionEnabled?.let { put("codeExecutionEnabled", it) }
    settings.googleSearchEnabled?.let { put("googleSearchEnabled", it) }
    settings.openAiWebSearchEnabled?.let { put("openAiWebSearchEnabled", it) }
    settings.thinkingEnabled?.let { put("thinkingEnabled", it) }
    settings.thinkingLevel?.let { put("thinkingLevel", it) }
    settings.thinkingBudgetEnabled?.let { put("thinkingBudgetEnabled", it) }
    settings.thinkingBudgetTokens?.let { put("thinkingBudgetTokens", it) }
    settings.openAiServiceTierEnabled?.let { put("openAiServiceTierEnabled", it) }
    settings.openAiServiceTier?.let { put("openAiServiceTier", it) }
    settings.webSearchEnabled?.let { put("webSearchEnabled", it) }
    settings.shellEnabled?.let { put("shellEnabled", it) }
    settings.lowContextModeEnabled?.let { put("lowContextModeEnabled", it) }
}

// ---- restored media -----------------------------------------------------------------------------

private fun parseRestoredMedia(element: JsonElement?): NativeConversationMediaRestorer.RestoredMedia {
    if (element !is JsonObject) {
        return NativeConversationMediaRestorer.RestoredMedia(emptyMap(), emptyMap(), emptyMap(), emptyList())
    }
    val archiveFiles = buildMap {
        (element["archiveFiles"] as? JsonObject)?.forEach { (name, value) ->
            val file = value.jsonObject
            put(
                name,
                NativeConversationMediaRestorer.RestoredMediaFile(
                    absolutePath = (file["absolutePath"] as JsonPrimitive).content,
                    uri = (file["uri"] as JsonPrimitive).content,
                ),
            )
        }
    }
    fun legacyIndex(name: String): Map<String, Map<Int, String>> =
        (element[name] as? JsonObject)?.mapValues { (_, value) ->
            (value as JsonObject).entries.associate { (key, entry) ->
                key.toInt() to (entry as JsonPrimitive).content
            }
        } ?: emptyMap()
    return NativeConversationMediaRestorer.RestoredMedia(
        archiveFiles = archiveFiles,
        legacyImagesByMessage = legacyIndex("legacyImagesByMessage"),
        legacyVideosByMessage = legacyIndex("legacyVideosByMessage"),
        createdFiles = emptyList(),
    )
}

// ---- table dump (the chat-dao stand's renderers, plus the import's own tables) ------------------

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
    put("conversation_settings_import_transfer", queryRows(
        connection,
        "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer " +
            "ORDER BY id",
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
        "SELECT kind, identity, state, revision, updatedAt FROM maintenance_debt " +
            "ORDER BY kind, identity",
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

// ---- masking ------------------------------------------------------------------------------------

private val EPOCH_MILLIS = Regex("(?<![0-9.])1[7-9][0-9]{11}(?![0-9])")

private fun maskTrace(text: String): String {
    // UUIDs: first-appearance ordinal replacement (fixture content carries no UUID-shaped
    // values; the generated ones are the settings-transfer id and planner-made run ids).
    val ordinals = LinkedHashMap<String, String>()
    val maskedUuids = UUID_PATTERN.replace(text) { match ->
        ordinals.getOrPut(match.value) { "<uuid:${ordinals.size}>" }
    }
    // Wall-clock stamps: conversations.dataChangedAt (the upsert's stamp) and the
    // maintenance/semantic updatedAt values are System.currentTimeMillis() reads — every
    // 13-digit epoch value becomes "<clock>" (fixtures never use such numbers).
    return EPOCH_MILLIS.replace(maskedUuids, "\"<clock>\"")
}

// ---- scenario runner ----------------------------------------------------------------------------

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

private fun render(scenarioFile: File): String = runBlocking {
    val root = Files.createTempDirectory("agora-graph-importer-").toFile()
    try {
        val scenario = scenarioJson.parseToJsonElement(scenarioFile.readText()).jsonObject
        val databaseFile = File(root, "db/agora_db").also { it.parentFile.mkdirs() }
        val filesDir = File(root, "files").also { it.mkdirs() }
        val cacheDir = File(root, "cache").also { it.mkdirs() }

        DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
            connection.createStatement().use { it.execute("PRAGMA foreign_keys = ON") }
            executeScript(connection, File("platform/schema.sql").readText())
            for (statement in scenario.stringList("seedSql")) {
                connection.createStatement().use { it.execute(statement) }
            }
        }

        val context = Context(
            databaseFile = databaseFile,
            filesDir = filesDir,
            cacheDir = cacheDir,
            packageName = "com.newoether.agora",
            packageManager = PackageManager(scenario.string("appVersion")),
            contentResolver = ContentResolver(),
        )
        // DataImporter's injected Json (the real call site constructs exactly this config).
        val importJson = Json { ignoreUnknownKeys = true; coerceInputValues = true }
        val mediaRestorer = NativeConversationMediaRestorer(context, importJson)
        val restoredMedia = parseRestoredMedia(scenario["restoredMedia"])

        var scheduled = false
        val database = ChatDatabase.build(context)
        val importer = NativeConversationGraphImporter(
            database = database,
            chatDao = database.chatDao(),
            importJson = importJson,
            mediaRestorer = mediaRestorer,
            scheduleMaintenance = { scheduled = true },
        )

        val archive = MapEntrySource(scenario.stringMap("entries"))
        val graphVersion = (scenario["graphVersion"] as? JsonPrimitive)?.content?.toIntOrNull() ?: 5
        val resolveMap: Map<String, String?> =
            (scenario["resolveSystemPromptIds"] as? JsonObject)?.let { mapping ->
                buildMap {
                    mapping.forEach { (key, value) ->
                        put(key, (value as? JsonPrimitive)?.contentOrNull)
                    }
                }
            } ?: emptyMap()
        fun resolveSystemPromptId(id: String?): String? {
            if (id == null) return null
            return if (resolveMap.containsKey(id)) resolveMap[id] else id
        }

        val restoredMediaSeed = restoredMedia
        val steps = mutableListOf<JsonElement>()

        for (element in scenario.stringJsonArray("steps")) {
            val step = element.jsonObject
            val op = step.string("op")
            val strategy = DataImporter.ImportStrategy.valueOf(
                step.stringOrNull("strategy") ?: scenario.stringOrNull("strategy") ?: "MERGE",
            )
            val result = try {
                when (op) {
                    "count" -> {
                        val source = NativeConversationGraphSource.open(archive, graphVersion, cacheDir)
                        try {
                            val counts = source.open().use { stream ->
                                importer.countConversationGraph(stream)
                            }
                            buildJsonObject {
                                put("conversations", counts.conversations)
                                put("tasks", counts.tasks)
                                put("loops", counts.loops)
                            }
                        } finally {
                            source.close()
                        }
                    }
                    "headers" -> {
                        val source = NativeConversationGraphSource.open(archive, graphVersion, cacheDir)
                        try {
                            val headers = source.open().use { stream ->
                                importer.readConversationGraphHeaders(
                                    stream = stream,
                                    strategy = strategy,
                                    restoredMedia = restoredMediaSeed,
                                    resolveSystemPromptId = ::resolveSystemPromptId,
                                )
                            }
                            headersJson(headers)
                        } finally {
                            source.close()
                        }
                    }
                    "import" -> {
                        scheduled = false
                        val source = NativeConversationGraphSource.open(archive, graphVersion, cacheDir)
                        try {
                            val headers = source.open().use { stream ->
                                importer.readConversationGraphHeaders(
                                    stream = stream,
                                    strategy = strategy,
                                    restoredMedia = restoredMediaSeed,
                                    resolveSystemPromptId = ::resolveSystemPromptId,
                                )
                            }
                            val semanticSnapshot: Pair<String?, Set<String>> =
                                (scenario["semanticSnapshot"] as? JsonArray)?.let { snapshot ->
                                    val modelId =
                                        (snapshot[0] as? JsonPrimitive)?.contentOrNull
                                    val conversations = (snapshot[1] as? JsonArray)
                                        ?.map { (it as JsonPrimitive).content }
                                        ?.toSet() ?: emptySet()
                                    modelId to conversations
                                } ?: (null as String? to emptySet())
                            val transferId = importer.importConversationGraph(
                                graphSource = source,
                                strategy = strategy,
                                headers = headers,
                                restoredMedia = restoredMediaSeed,
                                archiveVersion = (step["archiveVersion"] as? JsonPrimitive)?.content?.toIntOrNull() ?: 6,
                                semanticSnapshot = semanticSnapshot,
                            )
                            buildJsonObject {
                                put("transferId", transferId)
                                put("scheduled", scheduled)
                            }
                        } finally {
                            source.close()
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

            val stepObject = buildJsonObject {
                put("op", op)
                put("result", result)
                if (op == "import" && (result["error"] == null)) {
                    DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
                        put("tables", dumpTables(connection))
                    }
                }
            }
            steps.add(stepObject)
        }

        database.close()
        val trace = scenarioJson.encodeToString(
            JsonObject.serializer(),
            buildJsonObject { put("steps", JsonArray(steps)) },
        )
        maskTrace(trace)
    } finally {
        root.deleteRecursively()
    }
}

private fun JsonObject.stringMap(name: String): Map<String, String> =
    (this[name] as? JsonObject)?.mapValues { (it.value as JsonPrimitive).content } ?: emptyMap()

private fun JsonObject.stringJsonArray(name: String): JsonArray =
    this[name] as? JsonArray ?: JsonArray(emptyList())

private fun headersJson(
    headers: NativeConversationGraphImporter.ConversationGraphHeaders,
): JsonObject = buildJsonObject {
    put("tasks", buildJsonArray { headers.tasks.forEach { add(taskJson(it)) } })
    put("conversations", buildJsonArray { headers.conversations.forEach { add(conversationJson(it)) } })
    put("runs", buildJsonArray { headers.runs.forEach { add(runJson(it)) } })
    put("sourceRunIdsWereUnique", headers.sourceRunIdsWereUnique)
    put("loops", buildJsonArray { headers.loops.forEach { add(loopJson(it)) } })
    put(
        "availableConversationIds",
        buildJsonArray { headers.availableConversationIds.forEach { add(it) } },
    )
    put("conversationSettings", buildJsonObject {
        headers.conversationSettings.forEach { (id, settings) ->
            put(id, conversationSettingsJson(settings))
        }
    })
}
