// Golden trace driver for data/local/MaintenanceDebt.kt.
//
// The debt file, RunBranchSelectionIntegrity.kt, RunEntity.kt and RuntimeIdentity.kt compile
// verbatim. Room's generated DAO is replaced by the stand-local MiniRoom, which executes the
// declared @Query SQL on a real SQLite database over the stand schema (platform/schema.sql).
//
// Two documented boundaries:
//   * System.currentTimeMillis() inside repairRunBranches — the clock value written into
//     conversations.dataChangedAt is masked as "<clock>" (the port takes the timestamp as a
//     parameter), while values above any plausible clock are left visible to pin the
//     MAX(dataChangedAt + 1, :at) branch.
//   * ChatEntity is the stand-local conversation subset (platform/StandChat.kt).
package maintenance.debt.golden

import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.MaintenanceDebtDao
import com.newoether.agora.data.local.MaintenanceDebtEntity
import com.newoether.agora.data.local.MaintenanceDebtEntity.Companion.STATE_PENDING
import com.newoether.agora.data.local.RunEntity
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

/** The window a wall-clock timestamp lands in; scenario seeds sit outside it. */
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

        val database = com.newoether.agora.data.local.StandDebtDatabase(connection)
        database.attach(MiniRoom(database, connection))
        val dao = database.debtDao()

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
    dao: MaintenanceDebtDao,
    connection: Connection,
): JsonElement = when (op) {
    "insertDebt" -> JsonPrimitive(dao.insertDebt(debtOf(step["debt"]!!.jsonObject)))
    "renewDebt" -> JsonPrimitive(
        dao.renewDebt(step.string("kind"), step.string("identity"), step.long("updatedAt")),
    )
    "getDebt" ->
        dao.getDebt(step.string("kind"), step.string("identity"))?.let(::debtJson) ?: JsonNull
    "getClaimCandidates" -> JsonArray(
        dao.getClaimCandidates(step.long("staleBefore"), step.int("limit")).map(::debtJson),
    )
    "claimCandidate" -> JsonPrimitive(
        dao.claimCandidate(
            kind = step.string("kind"),
            identity = step.string("identity"),
            revision = step.long("revision"),
            claimId = step.string("claimId"),
            claimedAt = step.long("claimedAt"),
            staleBefore = step.long("staleBefore"),
        ),
    )
    "hasDebt" -> JsonPrimitive(dao.hasDebt())
    "getOrphanEmbeddingIdsPage" -> JsonArray(
        dao.getOrphanEmbeddingIdsPage(step.long("afterId"), step.int("limit"))
            .map { JsonPrimitive(it) },
    )
    "deleteOrphanEmbeddingsForMessage" -> JsonPrimitive(
        dao.deleteOrphanEmbeddingsForMessage(step.string("messageId")),
    )
    "deleteOrphanEmbeddingsByIds" -> JsonPrimitive(
        dao.deleteOrphanEmbeddingsByIds(
            step["ids"]!!.jsonArray.map { it.jsonPrimitive.content.toLong() },
        ),
    )
    "getRunBranchConversationIdsPage" -> JsonArray(
        dao.getRunBranchConversationIdsPage(step.optionalString("afterId"), step.int("limit"))
            .map { JsonPrimitive(it) },
    )
    "getMaintenanceConversation" ->
        dao.getMaintenanceConversation(step.string("conversationId"))
            ?.let(::conversationJson) ?: JsonNull
    "getMaintenanceRuns" -> JsonArray(
        dao.getMaintenanceRuns(step.string("conversationId")).map(::runJson),
    )
    "compareAndSetMaintenanceRunBranches" -> JsonPrimitive(
        dao.compareAndSetMaintenanceRunBranches(
            conversationId = step.string("conversationId"),
            expected = step.string("expected"),
            replacement = step.string("replacement"),
            at = step.long("at"),
        ),
    )
    "completeClaimed" -> JsonPrimitive(
        dao.completeClaimed(
            kind = step.string("kind"),
            identity = step.string("identity"),
            revision = step.long("revision"),
            claimId = step.string("claimId"),
        ),
    )
    "releaseClaimed" -> JsonPrimitive(
        dao.releaseClaimed(
            kind = step.string("kind"),
            identity = step.string("identity"),
            revision = step.long("revision"),
            claimId = step.string("claimId"),
            updatedAt = step.long("updatedAt"),
        ),
    )
    "enqueue" -> debtJson(
        dao.enqueue(step.string("kind"), step.string("identity"), step.long("updatedAt")),
    )
    "claim" -> JsonArray(
        dao.claim(
            claimId = step.string("claimId"),
            claimedAt = step.long("claimedAt"),
            staleBefore = step.long("staleBefore"),
            limit = step.int("limit"),
        ).map(::debtJson),
    )
    "repairRunBranches" -> JsonPrimitive(dao.repairRunBranches(step.string("conversationId")))
    "complete" -> JsonPrimitive(dao.complete(debtOf(step["debt"]!!.jsonObject)))
    "release" ->
        JsonPrimitive(dao.release(debtOf(step["debt"]!!.jsonObject), step.long("updatedAt")))
    "sql" -> JsonPrimitive(runSql(connection, step))
    else -> error("unknown operation $op")
}

// ---- scenario entities --------------------------------------------------------------------

private fun debtOf(source: JsonObject): MaintenanceDebtEntity = MaintenanceDebtEntity(
    kind = source.string("kind"),
    identity = source.string("identity"),
    state = source.optionalString("state") ?: STATE_PENDING,
    revision = source.optionalLong("revision") ?: 1L,
    updatedAt = source.optionalLong("updatedAt") ?: 0L,
    claimId = source.optionalString("claimId"),
    claimedAt = source.optionalLong("claimedAt"),
)

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
        connection.prepareStatement(
            "INSERT INTO messages (id, conversationId, text, participant) VALUES (?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.string("conversationId"))
            it.setString(3, row.optionalString("text") ?: "")
            it.setString(4, row.optionalString("participant") ?: "USER")
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

private fun debtJson(entity: MaintenanceDebtEntity): JsonElement = buildJsonObject {
    put("kind", entity.kind)
    put("identity", entity.identity)
    put("state", entity.state)
    put("revision", entity.revision)
    put("updatedAt", entity.updatedAt)
    put("claimId", entity.claimId)
    put("claimedAt", entity.claimedAt)
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

private fun conversationJson(entity: ChatEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("selectedRunBranchesJson", entity.selectedRunBranchesJson)
}

/** Values the repair's wall-clock wrote are masked; anything else prints verbatim. */
private fun clockMasked(value: Long): JsonElement =
    if (value in CLOCK_LOW until CLOCK_HIGH) JsonPrimitive("<clock>") else JsonPrimitive(value)

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("maintenance_debt", queryRows(
        connection,
        "SELECT kind, identity, state, revision, updatedAt, claimId, claimedAt " +
            "FROM maintenance_debt ORDER BY kind, identity",
    ) { row ->
        buildJsonObject {
            put("kind", row.getString(1))
            put("identity", row.getString(2))
            put("state", row.getString(3))
            put("revision", row.getLong(4))
            put("updatedAt", row.getLong(5))
            put("claimId", row.getString(6))
            put("claimedAt", row.nullableLong(7))
        }
    })
    put("conversations", queryRows(
        connection,
        "SELECT id, selectedRunBranchesJson, dataChangedAt FROM conversations ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getString(1))
            put("selectedRunBranchesJson", row.getString(2))
            put("dataChangedAt", clockMasked(row.getLong(3)))
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
}

/** JDBC's getLong collapses NULL to 0; keep the SQL NULL visible like getString does. */
private fun ResultSet.nullableLong(index: Int): JsonElement {
    val value = getLong(index)
    return if (wasNull()) JsonNull else JsonPrimitive(value)
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
