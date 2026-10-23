// Golden trace driver for data/local/ChatAutomationDao.kt.
//
// ChatAutomationDao.kt and ChatEntities.kt (TaskEntity, LoopEntity, the other entities and the
// type converters) compile VERBATIM from the app tree. Room's generated DAO is replaced by the
// stand-local MiniRoom: @Query runs on real SQLite over platform/schema.sql, @Upsert implements
// Room's insert-then-update-by-primary-key algorithm, and observed queries return cold,
// single-emission Flows (the stand has no invalidation bus).
//
// The trailing table dump shows both automation tables after every step, so Unit-returning writes
// and row-count CAS updates are observable.
package chat.automation.golden

import androidx.room.RoomDatabase
import com.newoether.agora.data.local.ChatAutomationDao
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.TaskEntity
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
        val dao = MiniRoom(database, connection).create(ChatAutomationDao::class.java)

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

// ---- operations (every ChatAutomationDao member) ------------------------------------------

private suspend fun executeOperation(
    op: String,
    step: JsonObject,
    dao: ChatAutomationDao,
): JsonElement = when (op) {
    "getAllTasks" -> JsonArray(dao.getAllTasks().first().map(::taskJson))
    "getTask" -> dao.getTask(step.string("id"))?.let(::taskJson) ?: JsonNull
    "getEnabledTasks" -> JsonArray(dao.getEnabledTasks().map(::taskJson))
    "upsertTask" -> {
        dao.upsertTask(task(step))
        JsonNull
    }
    "replaceTaskModelReferences" -> JsonPrimitive(
        dao.replaceTaskModelReferences(
            oldModelId = step.string("oldModelId"),
            newModelId = step.optionalString("newModelId"),
        ),
    )
    "renameTaskProviderModelReferences" -> JsonPrimitive(
        dao.renameTaskProviderModelReferences(
            oldProvider = step.string("oldProvider"),
            newProvider = step.string("newProvider"),
        ),
    )
    "updateTaskNextRunAtIfUnchanged" -> JsonPrimitive(
        dao.updateTaskNextRunAtIfUnchanged(
            id = step.string("id"),
            expectedCronExpr = step.string("expectedCronExpr"),
            expectedNextRunAt = step.long("expectedNextRunAt"),
            replacementNextRunAt = step.long("replacementNextRunAt"),
        ),
    )
    "deleteTask" -> {
        dao.deleteTask(step.string("id"))
        JsonNull
    }
    "deleteAllTasks" -> {
        dao.deleteAllTasks()
        JsonNull
    }
    "getAllTasksList" -> JsonArray(dao.getAllTasksList().map(::taskJson))
    "getLoop" -> dao.getLoop(step.string("conversationId")).first()?.let(::loopJson) ?: JsonNull
    "getActiveLoops" -> JsonArray(dao.getActiveLoops().map(::loopJson))
    "observeActiveLoops" -> JsonArray(dao.observeActiveLoops().first().map(::loopJson))
    "upsertLoopRow" -> {
        dao.upsertLoopRow(loop(step))
        JsonNull
    }
    "updateLoopNextFireAtIfUnchanged" -> JsonPrimitive(
        dao.updateLoopNextFireAtIfUnchanged(
            conversationId = step.string("conversationId"),
            expectedRevision = step.long("expectedRevision"),
            expectedCycleCount = step.int("expectedCycleCount"),
            expectedIntervalMs = step.long("expectedIntervalMs"),
            expectedNextFireAt = step.long("expectedNextFireAt"),
            replacementNextFireAt = step.long("replacementNextFireAt"),
        ),
    )
    "deactivateLoopRowIfUnchanged" -> JsonPrimitive(
        dao.deactivateLoopRowIfUnchanged(
            conversationId = step.string("conversationId"),
            expectedRevision = step.long("expectedRevision"),
            expectedCycleCount = step.int("expectedCycleCount"),
            expectedIntervalMs = step.long("expectedIntervalMs"),
            expectedNextFireAt = step.long("expectedNextFireAt"),
            normalizedMaxCycles = step.int("normalizedMaxCycles"),
        ),
    )
    "deleteLoopRow" -> JsonPrimitive(dao.deleteLoopRow(step.string("conversationId")))
    "deleteAllLoops" -> {
        dao.deleteAllLoops()
        JsonNull
    }
    "getAllLoopsList" -> JsonArray(dao.getAllLoopsList().map(::loopJson))
    "getLoopsForConversationSnapshot" -> JsonArray(
        dao.getLoopsForConversationSnapshot(step.string("conversationId")).map(::loopJson),
    )
    else -> error("unknown operation $op")
}

private fun task(step: JsonObject): TaskEntity = TaskEntity(
    id = step.string("id"),
    name = step.optionalString("name") ?: "",
    prompt = step.optionalString("prompt") ?: "",
    systemPrompt = step.optionalString("systemPrompt"),
    systemPromptId = step.optionalString("systemPromptId"),
    modelId = step.optionalString("modelId"),
    cronExpr = step.optionalString("cronExpr") ?: "",
    runAt = step.optionalLong("runAt"),
    nextRunAt = step.optionalLong("nextRunAt") ?: 0L,
    enabled = step.optionalBoolean("enabled") ?: true,
    createdAt = step.optionalLong("createdAt") ?: 0L,
    lastRunAt = step.optionalLong("lastRunAt"),
)

private fun loop(step: JsonObject): LoopEntity = LoopEntity(
    conversationId = step.string("conversationId"),
    intervalMs = step.optionalLong("intervalMs") ?: 0L,
    prompt = step.optionalString("prompt"),
    nextFireAt = step.optionalLong("nextFireAt") ?: 0L,
    cycleCount = step.optionalInt("cycleCount") ?: 0,
    maxCycles = step.optionalInt("maxCycles"),
    active = step.optionalBoolean("active") ?: true,
    revision = step.optionalLong("revision") ?: 0L,
)

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
    for (element in seed["tasks"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO tasks (id, name, prompt, systemPrompt, systemPromptId, modelId, " +
                "cronExpr, runAt, nextRunAt, enabled, createdAt, lastRunAt) " +
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("id"))
            it.setString(2, row.optionalString("name") ?: "")
            it.setString(3, row.optionalString("prompt") ?: "")
            it.setObject(4, row.optionalString("systemPrompt"))
            it.setObject(5, row.optionalString("systemPromptId"))
            it.setObject(6, row.optionalString("modelId"))
            it.setString(7, row.optionalString("cronExpr") ?: "")
            it.setObject(8, row.optionalLong("runAt"))
            it.setLong(9, row.optionalLong("nextRunAt") ?: 0L)
            it.setInt(10, if (row.optionalBoolean("enabled") != false) 1 else 0)
            it.setLong(11, row.optionalLong("createdAt") ?: 0L)
            it.setObject(12, row.optionalLong("lastRunAt"))
            it.executeUpdate()
        }
    }
    for (element in seed["loops"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        connection.prepareStatement(
            "INSERT INTO loops (conversationId, intervalMs, prompt, nextFireAt, cycleCount, " +
                "maxCycles, active, revision) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
        ).use {
            it.setString(1, row.string("conversationId"))
            it.setLong(2, row.optionalLong("intervalMs") ?: 0L)
            it.setObject(3, row.optionalString("prompt"))
            it.setLong(4, row.optionalLong("nextFireAt") ?: 0L)
            it.setInt(5, row.optionalInt("cycleCount") ?: 0)
            it.setObject(6, row.optionalInt("maxCycles"))
            it.setInt(7, if (row.optionalBoolean("active") != false) 1 else 0)
            it.setLong(8, row.optionalLong("revision") ?: 0L)
            it.executeUpdate()
        }
    }
}

// ---- rendering -----------------------------------------------------------------------------

private fun taskJson(entity: TaskEntity): JsonElement = buildJsonObject {
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

private fun loopJson(entity: LoopEntity): JsonElement = buildJsonObject {
    put("conversationId", entity.conversationId)
    put("intervalMs", entity.intervalMs)
    put("prompt", entity.prompt)
    put("nextFireAt", entity.nextFireAt)
    put("cycleCount", entity.cycleCount)
    put("maxCycles", entity.maxCycles)
    put("active", entity.active)
    put("revision", entity.revision)
}

private fun dumpTables(connection: Connection): JsonObject = buildJsonObject {
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

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.int(name: String): Int = string(name).toInt()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()
