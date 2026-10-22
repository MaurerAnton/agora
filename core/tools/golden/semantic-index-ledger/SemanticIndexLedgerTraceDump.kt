// Golden trace driver for data/local/SemanticIndexLedger.kt.
//
// The ledger file compiles verbatim. Room's generated DAO is replaced by the stand-local MiniRoom
// (platform/MiniRoom.kt) which executes the declared SQL on a real SQLite database, so the traces
// pin the SQL semantics and the @Transaction default-method sequences together. The stand schema
// (platform/schema.sql) mirrors the Room-generated DDL, and the sibling-table subset is documented
// there.
//
// Every step records its result (or the validation error it raises) and the scenario ends with a
// full dump of the five observed tables.
package semantic.ledger.golden

import com.newoether.agora.data.local.ChatDatabase
import com.newoether.agora.data.local.EmbeddingEntity
import com.newoether.agora.data.local.SemanticIndexDao
import com.newoether.agora.data.local.SemanticIndexLedgerEntity
import com.newoether.agora.data.local.SemanticIndexLedgerEntity.Companion.STATE_NEEDS_RECONCILE
import com.newoether.agora.data.local.SemanticIndexWorkEntity
import com.newoether.agora.data.local.ReconcileIndexableMessage
import com.newoether.agora.data.local.StandChatDatabase
import com.newoether.agora.data.local.commitSemanticEmbedding
import com.newoether.agora.data.local.deleteSemanticModel
import com.newoether.agora.data.local.invalidateSemanticModel
import com.newoether.agora.data.local.requestSemanticReconcile
import com.newoether.agora.data.local.semanticModelSnapshot
import com.newoether.agora.data.local.semanticSourceFingerprint
import com.newoether.agora.data.local.withSemanticEligibilityMutation
import com.newoether.agora.data.local.withSemanticGraphMutation
import com.newoether.agora.data.local.withSemanticSourceMutation
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
        val stmt = connection.createStatement()
        stmt.execute("PRAGMA foreign_keys = ON")
        stmt.close()
        val schema = File("platform/schema.sql").readText()
        // Strip all line comments (starting with --) before splitting.
        val noComments = schema.lines()
            .map { line ->
                val idx = line.indexOf("--")
                if (idx >= 0) line.substring(0, idx) else line
            }
            .joinToString("\n")
        for (statement in noComments.split(";")) {
            val trimmed = statement.trim()
            if (trimmed.isBlank()) continue

            val s2 = connection.createStatement()
            s2.execute(statement)
            s2.close()
        }
        seed(connection, root["seed"] as? JsonObject)

        val database = StandChatDatabase(connection)
        database.attach(MiniRoom(database, connection))

        val steps = mutableListOf<JsonElement>()
        val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
        for (element in stepList) {
            val step = element.jsonObject
            val op = step.string("op")
            val result = try {
                executeOperation(op, step, database, connection)
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", failure::class.simpleName ?: failure::class.java.name)
                    put("message", failure.message ?: "")
                }
            }
            steps.add(
                buildJsonObject {
                    put("op", op)
                    put("result", result)
                },
            )
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
    database: StandChatDatabase,
    connection: Connection,
): JsonElement {
    val dao = database.semanticIndexDao()
    return when (op) {
        "fingerprint" -> JsonPrimitive(semanticSourceFingerprint(step.string("text")))
        "snapshot" -> {
            val snapshot = snapshotOf(step)
            buildJsonObject {
                put("active", snapshot.first?.let(::JsonPrimitive) ?: JsonNull)
                put("configured", JsonArray(snapshot.second.map(::JsonPrimitive)))
            }
        }
        "admitModel" -> ledgerJson(dao.admitModel(step.string("modelId"), step.long("updatedAt")))
        "requestReconcile" ->
            ledgerJson(dao.requestReconcile(step.string("modelId"), step.long("updatedAt")))
        "enqueueExactWork" -> ledgerJson(
            dao.enqueueExactWork(
                modelId = step.string("modelId"),
                messageId = step.string("messageId"),
                sourceFingerprint = step.optionalString("sourceFingerprint"),
                updatedAt = step.long("updatedAt"),
            ),
        )
        "completeExactWork" ->
            JsonPrimitive(dao.completeExactWork(workOf(step["work"]!!.jsonObject), step.long("updatedAt")))
        "completeReconcile" -> JsonPrimitive(
            dao.completeReconcile(
                modelId = step.string("modelId"),
                expectedReconcileRevision = step.long("expectedReconcileRevision"),
                updatedAt = step.long("updatedAt"),
            ),
        )
        "insertLedger" -> JsonPrimitive(
            dao.insertLedger(ledgerOf(step["ledger"]!!.jsonObject)),
        )
        "getLedger" -> dao.getLedger(step.string("modelId"))?.let(::ledgerJson) ?: JsonNull
        "getLedgers" -> JsonArray(
            dao.getLedgers(step["modelIds"]!!.jsonArray.map { it.jsonPrimitive.content })
                .map(::ledgerJson),
        )
        "advanceForExactWork" -> JsonPrimitive(
            dao.advanceForExactWork(step.string("modelId"), step.long("updatedAt")),
        )
        "advanceForReconcile" -> JsonPrimitive(
            dao.advanceForReconcile(step.string("modelId"), step.long("updatedAt")),
        )
        "upsertWork" -> {
            dao.upsertWork(workOf(step["work"]!!.jsonObject))
            JsonNull
        }
        "getWork" -> dao.getWork(step.string("modelId"), step.string("messageId"))
            ?.let(::workJson) ?: JsonNull
        "getWorkPage" -> JsonArray(
            dao.getWorkPage(
                modelId = step.string("modelId"),
                maxSourceRevision = step.long("maxSourceRevision"),
                afterSourceRevision = step.long("afterSourceRevision"),
                afterMessageId = step.string("afterMessageId"),
                limit = step.int("limit"),
            ).map(::workJson),
        )
        "getWorkCountThroughRevision" -> JsonPrimitive(
            dao.getWorkCountThroughRevision(step.string("modelId"), step.long("maxSourceRevision")),
        )
        "getReconcileMessagesPage" -> JsonArray(
            dao.getReconcileMessagesPage(
                modelId = step.string("modelId"),
                maxWorkRevision = step.long("maxWorkRevision"),
                afterId = step.optionalString("afterId"),
                limit = step.int("limit"),
            ).map(::reconcileRowJson),
        )
        "getSearchableMessageText" ->
            dao.getSearchableMessageText(step.string("messageId"))?.let(::JsonPrimitive) ?: JsonNull
        "upsertEmbedding" -> {
            dao.upsertEmbedding(embeddingOf(step["embedding"]!!.jsonObject))
            JsonNull
        }
        "deleteEmbeddingsForMessages" -> JsonPrimitive(
            dao.deleteEmbeddingsForMessages(
                step["messageIds"]!!.jsonArray.map { it.jsonPrimitive.content },
            ),
        )
        "deleteEmbeddingsForModel" -> JsonPrimitive(
            dao.deleteEmbeddingsForModel(step.string("modelId")),
        )
        "deleteAllEmbeddings" -> JsonPrimitive(dao.deleteAllEmbeddings())
        "deleteWorkForModel" -> JsonPrimitive(dao.deleteWorkForModel(step.string("modelId")))
        "deleteMatchingWork" -> JsonPrimitive(
            dao.deleteMatchingWork(
                modelId = step.string("modelId"),
                messageId = step.string("messageId"),
                sourceFingerprint = step.optionalString("sourceFingerprint"),
                sourceRevision = step.long("sourceRevision"),
            ),
        )
        "markCurrentAfterExactWork" -> JsonPrimitive(
            dao.markCurrentAfterExactWork(
                modelId = step.string("modelId"),
                sourceRevision = step.long("sourceRevision"),
                updatedAt = step.long("updatedAt"),
            ),
        )
        "markAfterReconcile" -> JsonPrimitive(
            dao.markAfterReconcile(
                modelId = step.string("modelId"),
                expectedReconcileRevision = step.long("expectedReconcileRevision"),
                updatedAt = step.long("updatedAt"),
            ),
        )
        "deleteModel" -> JsonPrimitive(dao.deleteModel(step.string("modelId")))
        "requestSemanticReconcile" -> {
            dao.requestSemanticReconcile(snapshotOf(step), step.long("updatedAt"))
            JsonNull
        }
        "withSemanticSourceMutation" -> {
            database.withSemanticSourceMutation(
                snapshot = snapshotOf(step),
                messageIds = step["messageIds"]!!.jsonArray.map { it.jsonPrimitive.content },
                updatedAt = step.long("updatedAt"),
            ) {
                runSqlBlock(connection, step["blockSql"] as? JsonArray)
            }
            JsonNull
        }
        "withSemanticGraphMutation" -> {
            database.withSemanticGraphMutation(
                snapshot = snapshotOf(step),
                clearMessageIds = (step["clearMessageIds"] as? JsonArray)
                    ?.map { it.jsonPrimitive.content } ?: emptyList(),
                clearAllEmbeddings = step.boolean("clearAllEmbeddings"),
                updatedAt = step.long("updatedAt"),
            ) {
                runSqlBlock(connection, step["blockSql"] as? JsonArray)
            }
            JsonNull
        }
        "withSemanticEligibilityMutation" -> {
            database.withSemanticEligibilityMutation(
                snapshot = snapshotOf(step),
                conversationId = step.string("conversationId"),
                updatedAt = step.long("updatedAt"),
            ) {
                runSqlBlock(connection, step["blockSql"] as? JsonArray)
            }
            JsonNull
        }
        "commitSemanticEmbedding" -> JsonPrimitive(
            database.commitSemanticEmbedding(
                embedding = embeddingOf(step["embedding"]!!.jsonObject),
                expectedFingerprint = step.string("expectedFingerprint"),
                updatedAt = step.long("updatedAt"),
                expectedWorkRevision = step.optionalLong("expectedWorkRevision"),
                expectedReconcileRevision = step.optionalLong("expectedReconcileRevision"),
                completePendingWork = step.boolean("completePendingWork", default = true),
            ),
        )
        "invalidateSemanticModel" -> {
            database.invalidateSemanticModel(step.string("modelId"), step.long("updatedAt"))
            JsonNull
        }
        "deleteSemanticModel" -> {
            database.deleteSemanticModel(step.string("modelId"))
            JsonNull
        }
        "sql" -> JsonPrimitive(runSql(connection, step))
        else -> error("unknown operation $op")
    }
}

// ---- scenario helpers ---------------------------------------------------------------------

private fun snapshotOf(step: JsonObject): Pair<String?, Set<String>> = semanticModelSnapshot(
    activeModelId = step.string("activeModelId"),
    configuredModelIds = step["configured"]!!.jsonArray.map { it.jsonPrimitive.content },
)

private fun ledgerOf(source: JsonObject): SemanticIndexLedgerEntity = SemanticIndexLedgerEntity(
    modelId = source.string("modelId"),
    state = source.optionalString("state") ?: STATE_NEEDS_RECONCILE,
    sourceRevision = source.optionalLong("sourceRevision") ?: 0L,
    completedRevision = source.optionalLong("completedRevision") ?: 0L,
    reconcileRevision = source.optionalLong("reconcileRevision") ?: 0L,
    updatedAt = source.optionalLong("updatedAt") ?: 0L,
)

private fun workOf(source: JsonObject): SemanticIndexWorkEntity = SemanticIndexWorkEntity(
    modelId = source.string("modelId"),
    messageId = source.string("messageId"),
    sourceFingerprint = source.optionalString("sourceFingerprint"),
    sourceRevision = source.long("sourceRevision"),
    updatedAt = source.optionalLong("updatedAt") ?: 0L,
)

private fun embeddingOf(source: JsonObject): EmbeddingEntity = EmbeddingEntity(
    id = source.optionalLong("id") ?: 0L,
    messageId = source.string("messageId"),
    modelId = source.string("modelId"),
    embedding = hexToBytes(source.string("embedding")),
    chunkText = source.optionalString("chunkText") ?: "",
    dimension = source.optionalInt("dimension") ?: 0,
    sourceFingerprint = source.optionalString("sourceFingerprint"),
)

private fun runSqlBlock(connection: Connection, block: JsonArray?) {
    block ?: return
    for (element in block) {
        runSql(connection, element.jsonObject)
    }
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

private fun executeScript(connection: Connection, script: String) {
    for (statement in script.split(";")) {
        if (statement.isBlank()) continue
        val stmt = connection.createStatement()
        stmt.execute(statement)
        stmt.close()
    }
}

private fun seed(connection: Connection, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["conversations"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement("INSERT INTO conversations (id, taskId) VALUES (?, ?)").use {
            it.setString(1, row.string("id"))
            it.setObject(2, row.optionalString("taskId"))
            it.executeUpdate()
        }
    }
    for (element in seed["messages"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO messages (id, conversationId, text, participant) VALUES (?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.string("conversationId"))
            it.setString(3, row.string("text"))
            it.setString(4, row.string("participant"))
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

// ---- rendering ----------------------------------------------------------------------------

private fun ledgerJson(entity: SemanticIndexLedgerEntity): JsonElement = buildJsonObject {
    put("modelId", entity.modelId)
    put("state", entity.state)
    put("sourceRevision", entity.sourceRevision)
    put("completedRevision", entity.completedRevision)
    put("reconcileRevision", entity.reconcileRevision)
    put("updatedAt", entity.updatedAt)
}

private fun workJson(entity: SemanticIndexWorkEntity): JsonElement = buildJsonObject {
    put("modelId", entity.modelId)
    put("messageId", entity.messageId)
    put("sourceFingerprint", entity.sourceFingerprint)
    put("sourceRevision", entity.sourceRevision)
    put("updatedAt", entity.updatedAt)
}

private fun reconcileRowJson(row: ReconcileIndexableMessage): JsonElement = buildJsonObject {
    put("id", row.id)
    put("text", row.text)
    put("embeddingId", row.embeddingId)
    put("embeddingFingerprint", row.embeddingFingerprint)
    put("dimension", row.dimension)
    put("embeddingBytes", row.embeddingBytes)
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("semantic_index_ledger", queryRows(
        connection,
        "SELECT modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt " +
            "FROM semantic_index_ledger ORDER BY modelId",
    ) { row ->
        buildJsonObject {
            put("modelId", row.getString(1))
            put("state", row.getString(2))
            put("sourceRevision", row.getLong(3))
            put("completedRevision", row.getLong(4))
            put("reconcileRevision", row.getLong(5))
            put("updatedAt", row.getLong(6))
        }
    })
    put("semantic_index_work", queryRows(
        connection,
        "SELECT modelId, messageId, sourceFingerprint, sourceRevision, updatedAt " +
            "FROM semantic_index_work ORDER BY modelId, messageId",
    ) { row ->
        buildJsonObject {
            put("modelId", row.getString(1))
            put("messageId", row.getString(2))
            put("sourceFingerprint", row.getString(3))
            put("sourceRevision", row.getLong(4))
            put("updatedAt", row.getLong(5))
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
    put("messages", queryRows(
        connection,
        "SELECT id, conversationId, text, participant FROM messages ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("conversationId", row.getString(2))
            put("text", row.getString(3))
            put("participant", row.getString(4))
        }
    })
    put("conversations", queryRows(
        connection,
        "SELECT id, taskId FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("taskId", row.getString(2))
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

// ---- JSON helpers -------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.boolean(name: String, default: Boolean = false): Boolean =
    optionalString(name)?.toBooleanStrictOrNull() ?: default

private fun hexToBytes(hex: String): ByteArray {
    if (hex.isEmpty()) return ByteArray(0)
    return ByteArray(hex.length / 2) { index ->
        hex.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
}

private fun bytesToHex(bytes: ByteArray): String =
    bytes.joinToString("") { byte -> "%02x".format(byte) }
