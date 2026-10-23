// Golden trace driver for data/local/ChatSearchDao.kt.
//
// The DAO interface, ChatEntities.kt (all entities and the type converters it declares) and
// EmbeddingSearchRow.kt compile VERBATIM from the app tree. Room's generated DAO is replaced by
// the stand-local MiniRoom, which executes the declared @Query SQL on a real SQLite database over
// platform/schema.sql.
//
// Every one of the fourteen members is an operation, so no query goes unpinned. The DAO is
// read-only; the trailing table dump proves the queries never mutate the store and pins the full
// entity projection, not just the columns a particular ORDER BY happens to surface.
package chat.search.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.ChatSearchDao
import com.newoether.agora.data.local.EmbeddingModelCount
import com.newoether.agora.data.local.EmbeddingSearchRow
import com.newoether.agora.data.local.IndexableMessage
import com.newoether.agora.data.local.MessageEntity
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

        val database = object : RoomDatabase() {}
        val dao = MiniRoom(database, connection).create(ChatSearchDao::class.java)

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

// ---- operations (every ChatSearchDao member) ----------------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatSearchDao,
): JsonElement = when (op) {
    "searchMessages" -> JsonArray(
        dao.searchMessages(step.string("query"), step.optionalInt("limit") ?: 10)
            .map(::messageJson),
    )
    "getMessagesWithCitationSegmentsPage" -> JsonArray(
        dao.getMessagesWithCitationSegmentsPage(
            step.string("afterId"),
            step.int("pageSize"),
        ).map(::messageJson),
    )
    "getEmbeddingSearchPage" -> JsonArray(
        dao.getEmbeddingSearchPage(
            modelId = step.string("modelId"),
            afterId = step.long("afterId"),
            minimumTextLength = step.int("minimumTextLength"),
            limit = step.int("limit"),
        ).map(::embeddingSearchRowJson),
    )
    "getEmbeddingCountByModel" -> JsonPrimitive(
        dao.getEmbeddingCountByModel(step.string("modelId")),
    )
    "getEmbeddingCountsByModels" -> JsonArray(
        dao.getEmbeddingCountsByModels(
            step["modelIds"]!!.jsonArray.map { it.jsonPrimitive.content },
        ).map(::embeddingModelCountJson),
    )
    "getIndexableMessageCount" -> JsonPrimitive(dao.getIndexableMessageCount())
    "getSearchableMessagesPage" -> JsonArray(
        dao.getSearchableMessagesPage(
            step.optionalString("afterId"),
            step.int("limit"),
        ).map(::indexableMessageJson),
    )
    "getUnembeddedMessagesPage" -> JsonArray(
        dao.getUnembeddedMessagesPage(
            modelId = step.string("modelId"),
            afterId = step.optionalString("afterId"),
            limit = step.int("limit"),
        ).map(::indexableMessageJson),
    )
    "getSearchableMessagesByIds" -> JsonArray(
        dao.getSearchableMessagesByIds(
            step["ids"]!!.jsonArray.map { it.jsonPrimitive.content },
        ).map(::messageJson),
    )
    "isMessageSearchable" -> JsonPrimitive(
        dao.isMessageSearchable(step.string("messageId")),
    )
    "getSearchableConversation" ->
        dao.getSearchableConversation(step.string("conversationId"))
            ?.let(::conversationJson) ?: JsonNull
    "getSearchableConversationCount" -> JsonPrimitive(dao.getSearchableConversationCount())
    "getSearchableConversationsPageAscending" -> JsonArray(
        dao.getSearchableConversationsPageAscending(
            offset = step.int("offset"),
            limit = step.int("limit"),
        ).map(::conversationJson),
    )
    "getSearchableConversationsPageDescending" -> JsonArray(
        dao.getSearchableConversationsPageDescending(
            offset = step.int("offset"),
            limit = step.int("limit"),
        ).map(::conversationJson),
    )
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
            it.setString(2, row.string("title"))
            it.setLong(3, row.long("lastUpdated"))
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
        val images = when {
            row["images"] is JsonArray ->
                json.encodeToString(JsonArray.serializer(), row["images"]!!.jsonArray)
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
    for (element in seed["embeddings"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, " +
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setObject(1, row.optionalLong("id"))
            it.setString(2, row.string("messageId"))
            it.setString(3, row.string("modelId"))
            it.setBytes(4, hexToBytes(row.string("embedding")))
            it.setString(5, row.optionalString("chunkText") ?: "")
            it.setInt(6, row.optionalInt("dimension") ?: 0)
            it.setObject(7, row.optionalString("sourceFingerprint"))
            it.executeUpdate()
        }
    }
}

// ---- rendering -----------------------------------------------------------------------------

private fun conversationJson(entity: ChatEntity): JsonElement = buildJsonObject {
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

private fun messageJson(entity: MessageEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("conversationId", entity.conversationId)
    put("parentId", entity.parentId)
    put("text", entity.text)
    put("images", JsonArray(entity.images.map { JsonPrimitive(it) }))
    put("thoughts", entity.thoughts)
    put("thoughtTitle", entity.thoughtTitle)
    put("tokenCount", entity.tokenCount)
    put("inputTokenCount", entity.inputTokenCount)
    put("cachedInputTokenCount", entity.cachedInputTokenCount)
    put("cacheWriteInputTokenCount", entity.cacheWriteInputTokenCount)
    put("uncachedInputTokenCount", entity.uncachedInputTokenCount)
    put("outputTokenCount", entity.outputTokenCount)
    put("reasoningTokenCount", entity.reasoningTokenCount)
    put("generationDurationMs", entity.generationDurationMs)
    put("status", entity.status.name)
    put("participant", entity.participant.name)
    put("timestamp", entity.timestamp)
    put("thoughtTimeMs", entity.thoughtTimeMs)
    put("modelName", entity.modelName)
    put("toolCallJson", entity.toolCallJson)
    put("attachmentMeta", entity.attachmentMeta)
    put("runId", entity.runId)
    put("runSequence", entity.runSequence)
    put("consumedAtPass", entity.consumedAtPass)
    put("sourceJson", entity.sourceJson)
}

private fun embeddingSearchRowJson(entity: EmbeddingSearchRow): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("messageId", entity.messageId)
    put("embedding", bytesToHex(entity.embedding))
    put("dimension", entity.dimension)
}

private fun embeddingModelCountJson(entity: EmbeddingModelCount): JsonElement = buildJsonObject {
    put("modelId", entity.modelId)
    put("count", entity.count)
}

private fun indexableMessageJson(entity: IndexableMessage): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("text", entity.text)
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, systemPromptId, " +
            "modelId, taskId, origin, graduated, draftText, draftAttachments, " +
            "selectedRunBranchesJson, hasUnreadGeneration, isPinned " +
            "FROM conversations ORDER BY id",
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
            put("graduated", row.getLong(10) != 0L)
            put("draftText", row.getString(11))
            put("draftAttachments", row.getString(12))
            put("selectedRunBranchesJson", row.getString(13))
            put("hasUnreadGeneration", row.getLong(14) != 0L)
            put("isPinned", row.getLong(15) != 0L)
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
    put("embeddings", queryRows(
        connection,
        "SELECT id, messageId, modelId, embedding, chunkText, dimension, sourceFingerprint " +
            "FROM embeddings ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getLong(1))
            put("messageId", row.getString(2))
            put("modelId", row.getString(3))
            put("embedding", bytesToHex(row.getBytes(4)))
            put("chunkText", row.getString(5))
            put("dimension", row.getLong(6))
            put("sourceFingerprint", row.getString(7))
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

private fun hexToBytes(hex: String): ByteArray {
    if (hex.isEmpty()) return ByteArray(0)
    return ByteArray(hex.length / 2) { index ->
        hex.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}

private fun bytesToHex(bytes: ByteArray): String =
    bytes.joinToString("") { byte -> "%02x".format(byte) }
