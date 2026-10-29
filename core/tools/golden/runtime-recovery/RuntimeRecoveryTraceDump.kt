// Golden trace driver for data/local/ChatDaoRuntimeRecovery.kt.
//
// The recovery body, RunBranchSelectionIntegrity.kt, RunEntity.kt, RuntimeIdentity.kt and the
// lifted MessageEntity/MessageStreamCheckpoint/encodeSelectionMap compile from the app tree
// (`agora_extract recovery-sources`). Room's generated DAO is replaced by the stand-local MiniRoom,
// which executes the ChatDao query text on a real SQLite database over the stand schema.
//
// Documented boundaries: platform/ChatDao.kt transcribes the nine ChatDao queries the body calls,
// platform/DebugLog.kt drops the Android logging sink (timing instrumentation only), and
// platform/StandChat.kt is the observable conversation subset. `at` is a scenario parameter, so
// the trace is fully deterministic — no clock masking is needed here.
package runtime.recovery.golden

import com.newoether.agora.data.local.ChatDao
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.StandChatDatabase
import java.io.File
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
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
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

        val dao = StandChatDatabase(connection).also { database ->
            database.attach(MiniRoom(database, connection))
        }.chatDao()

        val steps = mutableListOf<JsonElement>()
        val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
        for (element in stepList) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                executeOperation(op, step, dao, connection)
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

// ---- operations ---------------------------------------------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatDao,
    connection: Connection,
): JsonElement = when (op) {
    "executeRuntimeRecovery" -> JsonPrimitive(
        dao.recoverConversationRuntime(step.string("conversationId"), step.long("at")),
    )
    "sql" -> JsonPrimitive(runSql(connection, step))
    else -> error("unknown operation $op")
}

private fun runSql(connection: Connection, step: JsonObject): Int {
    val arguments = step["args"] as? JsonArray ?: JsonArray(emptyList())
    return connection.prepareStatement(step.string("sql")).use { statement ->
        arguments.forEachIndexed { index, argument ->
            val primitive = argument as? JsonPrimitive
            when {
                primitive == null || primitive is JsonNull -> statement.setObject(index + 1, null)
                primitive.isString -> statement.setString(index + 1, primitive.content)
                else -> {
                    val text = primitive.content
                    if (text.contains('.') || text.contains('e', ignoreCase = true)) {
                        statement.setDouble(index + 1, text.toDouble())
                    } else {
                        statement.setLong(index + 1, text.toLong())
                    }
                }
            }
        }
        statement.executeUpdate()
    }
}

private fun seed(connection: Connection, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["conversations"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO conversations (id, selectedRunBranchesJson, dataChangedAt) VALUES (?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setObject(2, row.optionalString("selectedRunBranchesJson"))
            it.setLong(3, row.optionalLong("dataChangedAt") ?: 0L)
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
            it.setString(4, row.string("status"))
            it.setObject(5, row.optionalInt("activeSlot"))
            it.setLong(6, row.long("startedAt"))
            it.setLong(7, row.optionalLong("lastCheckpointAt") ?: row.long("startedAt"))
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
        // `images` seeds a JSON list (the converter's own encoding); `imagesRaw` seeds a stored
        // text verbatim, which is how the legacy "|||" format reaches the read path.
        val images = when {
            row["images"] is JsonArray -> json.encodeToString(
                JsonArray.serializer(),
                row["images"]!!.jsonArray,
            )
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
            it.setString(16, row.string("status"))
            it.setString(17, row.string("participant"))
            it.setLong(18, row.long("timestamp"))
            it.setObject(19, row.optionalLong("thoughtTimeMs"))
            it.setObject(20, row.optionalString("modelName"))
            it.setObject(21, row.optionalString("toolCallJson"))
            it.setObject(22, row.optionalString("attachmentMeta"))
            it.setString(23, row.string("runId"))
            it.setLong(24, row.optionalLong("runSequence") ?: MessageEntity.UNASSIGNED_RUN_SEQUENCE)
            it.setObject(25, row.optionalInt("consumedAtPass"))
            it.setObject(26, row.optionalString("sourceJson"))
            it.executeUpdate()
        }
    }
}

// ---- table dump ----------------------------------------------------------------------------

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, selectedRunBranchesJson, dataChangedAt FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("selectedRunBranchesJson", row.getString(2))
            put("dataChangedAt", row.nullableLong(3))
        }
    })
    put("runs", queryRows(
        connection,
        "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, " +
            "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, " +
            "legacyAmbiguous FROM runs ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentRunId", row.getString(3))
            put("status", row.getString(4))
            put("activeSlot", row.nullableLong(5))
            put("startedAt", row.getLong(6))
            put("lastCheckpointAt", row.getLong(7))
            put("stopRequestedAt", row.nullableLong(8))
            put("endedAt", row.nullableLong(9))
            put("endReason", row.getString(10))
            put("currentPass", row.getLong(11))
            put("legacyAmbiguous", row.getLong(12) != 0L)
        }
    })
    put("messages", queryRows(
        connection,
        "SELECT id, conversationId, parentId, text, images, thoughts, thoughtTitle, tokenCount, " +
            "inputTokenCount, cachedInputTokenCount, cacheWriteInputTokenCount, " +
            "uncachedInputTokenCount, outputTokenCount, reasoningTokenCount, " +
            "generationDurationMs, status, participant, timestamp, thoughtTimeMs, modelName, " +
            "toolCallJson, attachmentMeta, runId, runSequence, consumedAtPass, sourceJson " +
            "FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("parentId", row.getString(3))
            put("text", row.getString(4))
            put("images", row.getString(5))
            put("thoughts", row.getString(6))
            put("thoughtTitle", row.getString(7))
            put("tokenCount", row.getLong(8))
            put("inputTokenCount", row.nullableLong(9))
            put("cachedInputTokenCount", row.nullableLong(10))
            put("cacheWriteInputTokenCount", row.nullableLong(11))
            put("uncachedInputTokenCount", row.nullableLong(12))
            put("outputTokenCount", row.nullableLong(13))
            put("reasoningTokenCount", row.nullableLong(14))
            put("generationDurationMs", row.nullableLong(15))
            put("status", row.getString(16))
            put("participant", row.getString(17))
            put("timestamp", row.getLong(18))
            put("thoughtTimeMs", row.nullableLong(19))
            put("modelName", row.getString(20))
            put("toolCallJson", row.getString(21))
            put("attachmentMeta", row.getString(22))
            put("runId", row.getString(23))
            put("runSequence", row.getLong(24))
            put("consumedAtPass", row.nullableLong(25))
            put("sourceJson", row.getString(26))
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

/** JDBC's getLong collapses NULL to 0; keep the SQL NULL visible like getString does. */
private fun ResultSet.nullableLong(index: Int): JsonElement {
    val value = getLong(index)
    return if (wasNull()) JsonNull else JsonPrimitive(value)
}

// ---- JSON helpers -------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()
