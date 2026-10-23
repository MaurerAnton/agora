// Golden trace driver for data/local/NewChatPersistDao.kt.
//
// NewChatPersistDao.kt and ChatEntities.kt (NewChatPersistEntity, the other entities and the type
// converters) compile VERBATIM from the app tree. Room's generated DAO is replaced by the
// stand-local MiniRoom: @Query runs on real SQLite over platform/schema.sql, @Upsert implements
// Room's insert-then-update-by-primary-key algorithm, and the observed query returns a cold,
// single-emission Flow (the stand has no invalidation bus).
//
// The trailing table dump shows the singleton row after every step, so inserts, updates and
// deletes are observable even where the DAO returns Unit or a row count.
package new.chat.persist.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.NewChatPersistDao
import com.newoether.agora.data.local.NewChatPersistEntity
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
        val dao = MiniRoom(database, connection).create(NewChatPersistDao::class.java)

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

// ---- operations (every NewChatPersistDao member) ------------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: NewChatPersistDao,
): JsonElement = when (op) {
    "observeNewChatPersist" ->
        dao.observeNewChatPersist().first()?.let(::entityJson) ?: JsonNull
    "getNewChatPersist" -> dao.getNewChatPersist()?.let(::entityJson) ?: JsonNull
    "upsertNewChatPersist" -> {
        dao.upsertNewChatPersist(entity(step))
        JsonNull
    }
    "deleteNewChatPersist" -> JsonPrimitive(dao.deleteNewChatPersist())
    "deleteNewChatPersistIfMatches" -> JsonPrimitive(
        dao.deleteNewChatPersistIfMatches(
            modelId = step.optionalString("modelId"),
            systemPromptId = step.optionalString("systemPromptId"),
            conversationSettingsJson = step.optionalString("conversationSettingsJson"),
            draftText = step.optionalString("draftText") ?: "",
            draftAttachments = step.optionalString("draftAttachments"),
        ),
    )
    else -> error("unknown operation $op")
}

private fun entity(step: JsonObject): NewChatPersistEntity = NewChatPersistEntity(
    id = step.optionalInt("id") ?: 0,
    modelId = step.optionalString("modelId"),
    systemPromptId = step.optionalString("systemPromptId"),
    conversationSettingsJson = step.optionalString("conversationSettingsJson"),
    draftText = step.optionalString("draftText") ?: "",
    draftAttachments = step.optionalString("draftAttachments"),
)

// ---- seeding -------------------------------------------------------------------------------

private fun seed(connection: Connection, seed: JsonObject?) {
    if (seed == null) return
    for (element in seed["newChatPersist"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO new_chat_persist (id, modelId, systemPromptId, " +
                "conversationSettingsJson, draftText, draftAttachments) VALUES (?, ?, ?, ?, ?, ?)",
        ).use {
            it.setInt(1, row.optionalInt("id") ?: 0)
            it.setObject(2, row.optionalString("modelId"))
            it.setObject(3, row.optionalString("systemPromptId"))
            it.setObject(4, row.optionalString("conversationSettingsJson"))
            it.setString(5, row.optionalString("draftText") ?: "")
            it.setObject(6, row.optionalString("draftAttachments"))
            it.executeUpdate()
        }
    }
}

// ---- rendering -----------------------------------------------------------------------------

private fun entityJson(entity: NewChatPersistEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("modelId", entity.modelId)
    put("systemPromptId", entity.systemPromptId)
    put("conversationSettingsJson", entity.conversationSettingsJson)
    put("draftText", entity.draftText)
    put("draftAttachments", entity.draftAttachments)
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
    put("new_chat_persist", queryRows(
        connection,
        "SELECT id, modelId, systemPromptId, conversationSettingsJson, draftText, " +
            "draftAttachments FROM new_chat_persist ORDER BY id",
    ) { row ->
        buildJsonObject {
            put("id", row.getInt(1))
            put("modelId", row.getString(2))
            put("systemPromptId", row.getString(3))
            put("conversationSettingsJson", row.getString(4))
            put("draftText", row.getString(5))
            put("draftAttachments", row.getString(6))
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

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()
