// Golden trace driver for data/local/ChatProviderContextDao.kt.
//
// ChatProviderContextDao.kt and MessageContextTopology.kt compile VERBATIM from the app tree.
// Room's generated DAO is replaced by the stand-local MiniRoom, which executes the declared
// @Query SQL on a real SQLite database over platform/schema.sql, runs the @Transaction
// default-method body inside the stand database's transaction nesting, and returns a cold,
// single-emission Flow for the observed query (the stand has no invalidation bus).
//
// Every member is read-only; the trailing table dump proves the reads never mutate the store.
package chat.provider.context.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.ChatProviderContextDao
import com.newoether.agora.data.local.ConversationProviderContextState
import com.newoether.agora.data.local.MessageContextTopology
import com.newoether.agora.data.local.ProviderContextTopologySnapshot
import java.io.File
import java.sql.Connection
import java.sql.DriverManager
import java.sql.ResultSet
import kotlinx.coroutines.flow.first
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
import stand.room.MiniRoom

private val json = Json

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

private fun render(scenarioFile: File): String = runBlocking {
    val root = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val connection = DriverManager.getConnection("jdbc:sqlite::memory:")
    try {
        val pragma = connection.createStatement()
        pragma.execute("PRAGMA foreign_keys = ON")
        pragma.close()
        val schema = File("platform/schema.sql").readText()
        val statements = schema.lines()
            .map { line -> line.substringBefore("--") }
            .joinToString("\n")
            .split(";")
        for (statement in statements) {
            if (statement.isBlank()) continue
            val stmt = connection.createStatement()
            stmt.execute(statement)
            stmt.close()
        }
        seed(connection, root["seed"] as? JsonObject)

        val database = object : RoomDatabase() {}
        val dao = MiniRoom(database, connection).create(ChatProviderContextDao::class.java)

        val steps = mutableListOf<JsonElement>()
        val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
        for (element in stepList) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                executeOperation(op, step, dao)
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", failure::class.simpleName ?: failure::class.java.name)
                    put("message", failure.message ?: "")
                }
            }
            steps.add(buildJsonObject { put("op", op); put("result", result) })
        }

        val trace = buildJsonObject {
            put("steps", JsonArray(steps))
            put("tables", dumpTables(connection))
        }
        json.encodeToString(JsonObject.serializer(), trace)
    } finally {
        connection.close()
    }
}

// ---- operations (every ChatProviderContextDao member) -------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatProviderContextDao,
): JsonElement = when (op) {
    "getProviderContextState" ->
        dao.getProviderContextState(step.string("conversationId"))
            ?.let(::providerContextStateJson) ?: JsonNull
    "getMessageContextTopology" -> JsonArray(
        dao.getMessageContextTopology(step.string("conversationId"))
            .map(::messageContextTopologyJson),
    )
    "observeMessageContextTopology" -> JsonArray(
        dao.observeMessageContextTopology(step.string("conversationId")).first()
            .map(::messageContextTopologyJson),
    )
    "getProviderContextTopologySnapshot" ->
        dao.getProviderContextTopologySnapshot(step.string("conversationId"))
            ?.let(::providerContextTopologySnapshotJson) ?: JsonNull
    else -> error("unknown operation $op")
}

// ---- seeding -------------------------------------------------------------------------------

private fun seed(connection: Connection, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["conversations"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, " +
                "selectedBranchesJson, systemPromptId, modelId, taskId, origin, graduated, " +
                "draftText, draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, " +
                "isPinned) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.optionalString("title") ?: "")
            it.setLong(3, row.optionalLong("lastUpdated") ?: 0L)
            it.setLong(4, row.optionalLong("dataChangedAt") ?: 0L)
            it.setObject(5, row.optionalString("selectedBranchesJson"))
            it.setObject(6, row.optionalString("systemPromptId"))
            it.setObject(7, row.optionalString("modelId"))
            it.setObject(8, row.optionalString("taskId"))
            it.setString(9, row.optionalString("origin") ?: "user")
            it.setInt(10, if (row.optionalBoolean("graduated") == true) 1 else 0)
            it.setString(11, row.optionalString("draftText") ?: "")
            it.setObject(12, row.optionalString("draftAttachments"))
            it.setObject(13, row.optionalString("selectedRunBranchesJson"))
            it.setInt(14, if (row.optionalBoolean("hasUnreadGeneration") == true) 1 else 0)
            it.setInt(15, if (row.optionalBoolean("isPinned") == true) 1 else 0)
            it.executeUpdate()
        }
    }
    for (element in seed["runs"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, " +
                "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, " +
                "legacyAmbiguous) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.string("conversationId"))
            it.setObject(3, row.optionalString("parentRunId"))
            it.setString(4, row.optionalString("status") ?: "COMPLETED")
            it.setObject(5, row.optionalInt("activeSlot"))
            it.setLong(6, row.optionalLong("startedAt") ?: 0L)
            it.setLong(7, row.optionalLong("lastCheckpointAt") ?: 0L)
            it.setObject(8, row.optionalLong("stopRequestedAt"))
            it.setObject(9, row.optionalLong("endedAt"))
            it.setObject(10, row.optionalString("endReason"))
            it.setInt(11, row.optionalInt("currentPass") ?: 0)
            it.setInt(12, if (row.optionalBoolean("legacyAmbiguous") == true) 1 else 0)
            it.executeUpdate()
        }
    }
    for (element in seed["messages"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        val images = when {
            row["images"] is JsonArray ->
                json.encodeToString(JsonArray.serializer(), row["images"]!! as JsonArray)
            row["imagesRaw"] is JsonPrimitive -> row.string("imagesRaw")
            else -> "[]"
        }
        connection.prepareStatement(
            "INSERT INTO messages (id, conversationId, parentId, text, images, thoughts, " +
                "thoughtTitle, tokenCount, inputTokenCount, cachedInputTokenCount, " +
                "cacheWriteInputTokenCount, uncachedInputTokenCount, outputTokenCount, " +
                "reasoningTokenCount, generationDurationMs, status, participant, timestamp, " +
                "thoughtTimeMs, modelName, toolCallJson, attachmentMeta, runId, runSequence, " +
                "consumedAtPass, sourceJson) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, " +
                "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.string("conversationId"))
            it.setObject(3, row.optionalString("parentId"))
            it.setString(4, row.optionalString("text") ?: "")
            it.setString(5, images)
            it.setObject(6, row.optionalString("thoughts"))
            it.setObject(7, row.optionalString("thoughtTitle"))
            it.setInt(8, row.optionalInt("tokenCount") ?: 0)
            it.setObject(9, row.optionalInt("inputTokenCount"))
            it.setObject(10, row.optionalInt("cachedInputTokenCount"))
            it.setObject(11, row.optionalInt("cacheWriteInputTokenCount"))
            it.setObject(12, row.optionalInt("uncachedInputTokenCount"))
            it.setObject(13, row.optionalInt("outputTokenCount"))
            it.setObject(14, row.optionalInt("reasoningTokenCount"))
            it.setObject(15, row.optionalLong("generationDurationMs"))
            it.setString(16, row.optionalString("status") ?: "SUCCESS")
            it.setString(17, row.optionalString("participant") ?: "MODEL")
            it.setLong(18, row.optionalLong("timestamp") ?: 0L)
            it.setObject(19, row.optionalLong("thoughtTimeMs"))
            it.setObject(20, row.optionalString("modelName"))
            it.setObject(21, row.optionalString("toolCallJson"))
            it.setObject(22, row.optionalString("attachmentMeta"))
            it.setString(23, row.optionalString("runId") ?: "r1")
            it.setLong(24, row.optionalLong("runSequence") ?: -1L)
            it.setObject(25, row.optionalInt("consumedAtPass"))
            it.setObject(26, row.optionalString("sourceJson"))
            it.executeUpdate()
        }
    }
}

// ---- rendering -----------------------------------------------------------------------------

private fun messageContextTopologyJson(entity: MessageContextTopology): JsonElement =
    buildJsonObject {
        put("id", entity.id)
        put("conversationId", entity.conversationId)
        put("parentId", entity.parentId)
        put("status", entity.status.name)
        put("participant", entity.participant.name)
        put("timestamp", entity.timestamp)
        put("tokenCount", entity.tokenCount)
        put("modelName", entity.modelName)
        put("runId", entity.runId)
        put("runSequence", entity.runSequence)
        put("consumedAtPass", entity.consumedAtPass)
    }

private fun providerContextStateJson(entity: ConversationProviderContextState): JsonElement =
    buildJsonObject {
        put("selectedBranchesJson", entity.selectedBranchesJson)
    }

private fun providerContextTopologySnapshotJson(
    entity: ProviderContextTopologySnapshot,
): JsonElement = buildJsonObject {
    put("selectedBranchesJson", entity.selectedBranchesJson)
    put("messages", JsonArray(entity.messages.map(::messageContextTopologyJson)))
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, selectedBranchesJson FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("selectedBranchesJson", row.getString(2))
        }
    })
    put("messages", queryRows(
        connection,
        "SELECT id, conversationId, parentId, status, participant, timestamp, tokenCount, " +
            "modelName, runId, runSequence, consumedAtPass FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentId", row.getString(3))
            put("status", row.getString(4))
            put("participant", row.getString(5))
            put("timestamp", row.getLong(6))
            put("tokenCount", row.getInt(7))
            put("modelName", row.getString(8))
            put("runId", row.getString(9))
            put("runSequence", row.getLong(10))
            put("consumedAtPass", (row.getObject(11) as? Number)?.toInt())
        }
    })
}

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

// ---- JSON helpers --------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()
