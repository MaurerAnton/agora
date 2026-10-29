// Golden trace driver for data/local/ChatContextCompactDao.kt.
//
// The compact-graph interface, the lifted MessageEntity, RunEntity.kt, RuntimeIdentity.kt and the
// ChatMessage model cluster compile from the app tree (`agora_extract message-entity`). Room's
// generated DAO is replaced by the stand-local MiniRoom, which executes the declared @Query SQL on
// a real SQLite database over the stand schema; both @Transaction bodies run under Room's
// begin/set-success/end stack.
//
// Every method of the interface is an operation, so no query goes unpinned. Two documented
// boundaries:
//   * System.currentTimeMillis() inside removeContextCompact — the lastUpdated/dataChangedAt
//     values it writes are masked as "<clock>" (the port takes the timestamp as a parameter),
//     while scenario-supplied `at` values sit outside the window and print verbatim.
//   * ChatEntity is the stand-local conversation subset (platform/StandChat.kt).
package context.compact.golden

import com.newoether.agora.data.local.ChatContextCompactDao
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.StandChatDatabase
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
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

/** The window a wall-clock timestamp lands in; scenario-supplied `at` values sit outside it. */
private const val CLOCK_LOW = 1_000_000_000_000L
private const val CLOCK_HIGH = 4_000_000_000_000L

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

        val database = StandChatDatabase(connection)
        database.attach(MiniRoom(database, connection))
        val dao = database.compactDao()

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

// ---- operations (every ChatContextCompactDao method) --------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatContextCompactDao,
    connection: Connection,
): JsonElement = when (op) {
    "getConversation" -> dao.getConversation(step.string("conversationId"))
        ?.let(::conversationJson) ?: JsonNull
    "insertMessage" -> {
        dao.insertMessage(messageOf(step["message"]!!.jsonObject))
        JsonNull
    }
    "getMessage" -> dao.getMessage(step.string("messageId"))?.let(::messageJson) ?: JsonNull
    "insertRun" -> {
        dao.insertRun(runOf(step["run"]!!.jsonObject))
        JsonNull
    }
    "getRun" -> dao.getRun(step.string("runId"))?.let(::runJson) ?: JsonNull
    "getMessagesForRuns" -> JsonArray(
        dao.getMessagesForRuns(
            step["runIds"]!!.jsonArray.map { it.jsonPrimitive.content },
        ).map(::messageJson),
    )
    "deleteRun" -> JsonPrimitive(dao.deleteRun(step.string("runId")))
    "deleteEmbeddingsByMessageIds" -> {
        dao.deleteEmbeddingsByMessageIds(
            step["messageIds"]!!.jsonArray.map { it.jsonPrimitive.content },
        )
        JsonNull
    }
    "updateSelectionsForRunDeletion" -> JsonPrimitive(
        dao.updateSelectionsForRunDeletion(
            conversationId = step.string("conversationId"),
            selectedBranchesJson = step.string("selectedBranchesJson"),
            selectedRunBranchesJson = step.string("selectedRunBranchesJson"),
            at = step.long("at"),
        ),
    )
    "updateMessageBranchSelections" -> JsonPrimitive(
        dao.updateMessageBranchSelections(
            conversationId = step.string("conversationId"),
            selectedBranchesJson = step.string("selectedBranchesJson"),
            at = step.long("at"),
        ),
    )
    "updateRunBranchSelections" -> JsonPrimitive(
        dao.updateRunBranchSelections(
            conversationId = step.string("conversationId"),
            selectedRunBranchesJson = step.string("selectedRunBranchesJson"),
            at = step.long("at"),
        ),
    )
    "updateBranchSelections" -> JsonPrimitive(
        dao.updateBranchSelections(
            conversationId = step.string("conversationId"),
            selectedBranchesJson = step.string("selectedBranchesJson"),
            selectedRunBranchesJson = step.string("selectedRunBranchesJson"),
            at = step.long("at"),
        ),
    )
    "getLiveRun" -> dao.getLiveRun(step.string("conversationId"))?.let(::runJson) ?: JsonNull
    "nextRunSequence" -> JsonPrimitive(dao.nextRunSequence(step.string("runId")))
    "touchRun" -> JsonPrimitive(dao.touchRun(step.string("runId"), step.long("at")))
    "deleteMessagesByIds" -> {
        dao.deleteMessagesByIds(step["ids"]!!.jsonArray.map { it.jsonPrimitive.content })
        JsonNull
    }
    "reparentMessageChildren" -> JsonPrimitive(
        dao.reparentMessageChildren(
            removedMessageId = step.string("removedMessageId"),
            replacementParentId = step.optionalString("replacementParentId"),
        ),
    )
    "reparentRunChildren" -> JsonPrimitive(
        dao.reparentRunChildren(
            removedRunId = step.string("removedRunId"),
            replacementParentRunId = step.optionalString("replacementParentRunId"),
        ),
    )
    "replaceContextCompactMessageRun" -> JsonPrimitive(
        dao.replaceContextCompactMessageRun(
            messageId = step.string("messageId"),
            oldRunId = step.string("oldRunId"),
            newRunId = step.string("newRunId"),
            modelName = step.string("modelName"),
        ),
    )
    "beginRecompactContextCompact" -> messageJson(
        dao.beginRecompactContextCompact(
            replacementRun = runOf(step["replacementRun"]!!.jsonObject),
            messageId = step.string("messageId"),
            modelName = step.string("modelName"),
            expectedSelectedBranchesJson = step.string("expectedSelectedBranchesJson"),
        ),
    )
    "removeContextCompact" ->
        JsonPrimitive(dao.removeContextCompact(step.string("messageId")))
    "sql" -> JsonPrimitive(runSql(connection, step))
    else -> error("unknown operation $op")
}

private fun runSql(connection: Connection, step: JsonObject): Int {
    val sql = step.string("sql")
    // PRAGMA statements take no bind arguments and produce no row count; the scenarios use them
    // to reach the guards SQLite's foreign keys would otherwise make structurally unreachable.
    if (sql.trimStart().startsWith("PRAGMA", ignoreCase = true)) {
        connection.createStatement().use { it.execute(sql) }
        return 0
    }
    val arguments = step["args"] as? JsonArray ?: JsonArray(emptyList())
    return connection.prepareStatement(sql).use { statement ->
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

// ---- scenario entities (Kotlin constructors run their init blocks here) --------------------

private fun runOf(source: JsonObject): RunEntity {
    val status = runStatus(source.string("status"))
    val endReason = source.optionalString("endReason")?.let(::runEndReason)
    return RunEntity(
        id = source.string("id"),
        conversationId = source.string("conversationId"),
        parentRunId = source.optionalString("parentRunId"),
        status = status,
        activeSlot = source.optionalInt("activeSlot"),
        startedAt = source.long("startedAt"),
        lastCheckpointAt = source.optionalLong("lastCheckpointAt") ?: source.long("startedAt"),
        stopRequestedAt = source.optionalLong("stopRequestedAt"),
        endedAt = source.optionalLong("endedAt"),
        endReason = endReason,
        currentPass = source.optionalInt("currentPass") ?: 0,
        legacyAmbiguous = source.optionalBoolean("legacyAmbiguous") ?: false,
    )
}

private fun messageOf(source: JsonObject): MessageEntity = MessageEntity(
    id = source.string("id"),
    conversationId = source.string("conversationId"),
    parentId = source.optionalString("parentId"),
    text = source.optionalString("text") ?: "",
    images = (source["images"] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList(),
    thoughts = source.optionalString("thoughts"),
    thoughtTitle = source.optionalString("thoughtTitle"),
    tokenCount = source.optionalInt("tokenCount") ?: 0,
    inputTokenCount = source.optionalInt("inputTokenCount"),
    cachedInputTokenCount = source.optionalInt("cachedInputTokenCount"),
    cacheWriteInputTokenCount = source.optionalInt("cacheWriteInputTokenCount"),
    uncachedInputTokenCount = source.optionalInt("uncachedInputTokenCount"),
    outputTokenCount = source.optionalInt("outputTokenCount"),
    reasoningTokenCount = source.optionalInt("reasoningTokenCount"),
    generationDurationMs = source.optionalLong("generationDurationMs"),
    status = messageStatus(source.optionalString("status") ?: "SUCCESS"),
    participant = participant(source.optionalString("participant") ?: "MODEL"),
    timestamp = source.long("timestamp"),
    thoughtTimeMs = source.optionalLong("thoughtTimeMs"),
    modelName = source.optionalString("modelName"),
    toolCallJson = source.optionalString("toolCallJson"),
    attachmentMeta = source.optionalString("attachmentMeta"),
    runId = source.string("runId"),
    runSequence = source.optionalLong("runSequence") ?: MessageEntity.UNASSIGNED_RUN_SEQUENCE,
    consumedAtPass = source.optionalInt("consumedAtPass"),
    sourceJson = source.optionalString("sourceJson"),
)

private fun runStatus(name: String): RunStatus = when (name) {
    "ACTIVE" -> RunStatus.ACTIVE
    "STOPPING" -> RunStatus.STOPPING
    "COMPLETED" -> RunStatus.COMPLETED
    "STOPPED" -> RunStatus.STOPPED
    "FAILED" -> RunStatus.FAILED
    else -> throw IllegalArgumentException(
        "No enum constant com.newoether.agora.model.RunStatus.$name",
    )
}

private fun runEndReason(name: String): RunEndReason = when (name) {
    "MODEL_COMPLETED" -> RunEndReason.MODEL_COMPLETED
    "USER_STOPPED" -> RunEndReason.USER_STOPPED
    "PROCESS_RECOVERED" -> RunEndReason.PROCESS_RECOVERED
    "PROVIDER_ERROR" -> RunEndReason.PROVIDER_ERROR
    else -> throw IllegalArgumentException(
        "No enum constant com.newoether.agora.model.RunEndReason.$name",
    )
}

private fun messageStatus(name: String): com.newoether.agora.model.MessageStatus = when (name) {
    "TRANSCRIBING" -> com.newoether.agora.model.MessageStatus.TRANSCRIBING
    "SENDING" -> com.newoether.agora.model.MessageStatus.SENDING
    "THINKING" -> com.newoether.agora.model.MessageStatus.THINKING
    "TOOL_CALLING" -> com.newoether.agora.model.MessageStatus.TOOL_CALLING
    "SUCCESS" -> com.newoether.agora.model.MessageStatus.SUCCESS
    "STOPPED" -> com.newoether.agora.model.MessageStatus.STOPPED
    "ERROR" -> com.newoether.agora.model.MessageStatus.ERROR
    else -> throw IllegalArgumentException(
        "No enum constant com.newoether.agora.model.MessageStatus.$name",
    )
}

private fun participant(name: String): com.newoether.agora.model.Participant = when (name) {
    "USER" -> com.newoether.agora.model.Participant.USER
    "MODEL" -> com.newoether.agora.model.Participant.MODEL
    "ERROR" -> com.newoether.agora.model.Participant.ERROR
    else -> throw IllegalArgumentException(
        "No enum constant com.newoether.agora.model.Participant.$name",
    )
}

// ---- seeding -------------------------------------------------------------------------------

private fun seed(connection: Connection, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["conversations"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO conversations (id, selectedBranchesJson, selectedRunBranchesJson, " +
                "lastUpdated, dataChangedAt) VALUES (?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setObject(2, row.optionalString("selectedBranchesJson"))
            it.setObject(3, row.optionalString("selectedRunBranchesJson"))
            it.setLong(4, row.optionalLong("lastUpdated") ?: 0L)
            it.setLong(5, row.optionalLong("dataChangedAt") ?: 0L)
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
    put("selectedBranchesJson", entity.selectedBranchesJson)
    put("selectedRunBranchesJson", entity.selectedRunBranchesJson)
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

private fun runJson(entity: RunEntity): JsonElement = buildJsonObject {
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

/** Values the wall clock wrote are masked; scenario-supplied `at` values print verbatim. */
private fun clockMasked(value: Long): JsonElement =
    if (value in CLOCK_LOW until CLOCK_HIGH) JsonPrimitive("<clock>") else JsonPrimitive(value)

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("conversations", queryRows(
        connection,
        "SELECT id, selectedBranchesJson, selectedRunBranchesJson, lastUpdated, dataChangedAt " +
            "FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("selectedBranchesJson", row.getString(2))
            put("selectedRunBranchesJson", row.getString(3))
            put("lastUpdated", clockMasked(row.getLong(4)))
            put("dataChangedAt", clockMasked(row.getLong(5)))
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
