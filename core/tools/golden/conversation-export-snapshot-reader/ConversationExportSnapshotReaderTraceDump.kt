// Golden trace driver for data/ConversationExportSnapshotReader.kt.
//
// The reader and every entity/model file it references compile VERBATIM from the app tree. The
// only substitutes are the Android Context/Process hints and the Room reader-connection surface:
// ChatDatabase.build, useReaderConnection and Transactor run over a real SQLite JDBC database,
// while the reader's own traversal, paging, includeBody and task-tail sequence stay in the app
// source.
package conversation.export.snapshot.golden

import android.content.Context
import com.newoether.agora.data.ConversationExportSnapshotReader
import com.newoether.agora.data.SnapshotRecord
import com.newoether.agora.data.local.ChatEntity
import com.newoether.agora.data.local.LoopEntity
import com.newoether.agora.data.local.MessageEntity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.data.local.TaskEntity
import java.io.File
import java.nio.file.Files
import java.sql.Connection
import java.sql.DriverManager
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

private val json = Json { ignoreUnknownKeys = true }

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

private fun render(scenarioFile: File): String = runBlocking {
    val root = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val directory = Files.createTempDirectory("agora-export-reader-").toFile()
    val databaseFile = File(directory, "agora_db")
    try {
        DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
            connection.createStatement().use { it.execute("PRAGMA foreign_keys = ON") }
            executeScript(connection, File("platform/schema.sql").readText())
            for (element in root["seedSql"] as? JsonArray ?: JsonArray(emptyList())) {
                connection.createStatement().use { it.execute(element.jsonPrimitive.content) }
            }
        }

        val reader = ConversationExportSnapshotReader(Context(databaseFile))
        val steps = mutableListOf<JsonElement>()
        for (element in root["steps"] as? JsonArray ?: JsonArray(emptyList())) {
            val step = element.jsonObject
            val op = step.string("op")
            val skipBody = (step["skipBody"] as? JsonArray)
                ?.map { it.jsonPrimitive.content }
                ?.toSet()
                ?: emptySet()
            val failIncludeBody = step.optionalString("failIncludeBody")
            val failOnRecordAt = step.optionalInt("failOnRecordAt")
            val records = mutableListOf<JsonElement>()
            val includeBodyCalls = mutableListOf<String>()
            val result = try {
                reader.readSnapshot(
                    includeBody = { conversation ->
                        includeBodyCalls.add(conversation.id)
                        if (conversation.id == failIncludeBody) {
                            error("includeBody failed for ${conversation.id}")
                        }
                        conversation.id !in skipBody
                    },
                    onRecord = { record ->
                        if (records.size == failOnRecordAt) {
                            error("onRecord failed at ${records.size}")
                        }
                        records.add(recordJson(record))
                    },
                )
                buildJsonObject {
                    put("records", JsonArray(records))
                    put("includeBodyCalls", JsonArray(includeBodyCalls.map(::JsonPrimitive)))
                }
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", failure::class.simpleName ?: failure::class.java.name)
                    put("message", failure.message ?: "")
                    put("records", JsonArray(records))
                    put("includeBodyCalls", JsonArray(includeBodyCalls.map(::JsonPrimitive)))
                }
            }
            steps.add(buildJsonObject { put("op", op); put("result", result) })
        }

        val trace = buildJsonObject {
            put("steps", JsonArray(steps))
            put("tables", dumpKeys(databaseFile))
        }
        json.encodeToString(JsonObject.serializer(), trace)
    } finally {
        directory.deleteRecursively()
    }
}

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

private fun dumpKeys(databaseFile: File): JsonObject =
    DriverManager.getConnection("jdbc:sqlite:${databaseFile.path}").use { connection ->
        fun ids(sql: String): JsonArray = buildJsonArray {
            connection.createStatement().use { statement ->
                statement.executeQuery(sql).use { rows ->
                    while (rows.next()) add(JsonPrimitive(rows.getString(1)))
                }
            }
        }
        buildJsonObject {
            put("conversations", ids("SELECT id FROM conversations ORDER BY rowid"))
            put("runs", ids("SELECT id FROM runs ORDER BY rowid"))
            put("messages", ids("SELECT id FROM messages ORDER BY rowid"))
            put("loops", ids("SELECT conversationId FROM loops ORDER BY rowid"))
            put("tasks", ids("SELECT id FROM tasks ORDER BY rowid"))
        }
    }

private fun buildJsonArray(block: kotlinx.serialization.json.JsonArrayBuilder.() -> Unit): JsonArray =
    kotlinx.serialization.json.buildJsonArray(block)

private fun recordJson(record: SnapshotRecord): JsonElement = when (record) {
    is SnapshotRecord.Conversation -> buildJsonObject {
        put("type", "conversation")
        put("entity", conversationJson(record.entity))
    }
    is SnapshotRecord.Run -> buildJsonObject {
        put("type", "run")
        put("entity", runJson(record.entity))
    }
    is SnapshotRecord.Message -> buildJsonObject {
        put("type", "message")
        put("entity", messageJson(record.entity))
    }
    is SnapshotRecord.Loop -> buildJsonObject {
        put("type", "loop")
        put("entity", loopJson(record.entity))
    }
    is SnapshotRecord.Task -> buildJsonObject {
        put("type", "task")
        put("entity", taskJson(record.entity))
    }
}

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

private fun messageJson(entity: MessageEntity): JsonElement = buildJsonObject {
    put("id", entity.id)
    put("conversationId", entity.conversationId)
    put("parentId", entity.parentId)
    put("text", entity.text)
    put("images", JsonArray(entity.images.map(::JsonPrimitive)))
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

private fun JsonObject.string(name: String): String =
    this[name]?.jsonPrimitive?.content ?: error("missing $name")

private fun JsonObject.optionalString(name: String): String? =
    this[name]?.takeIf { it !is JsonNull }?.jsonPrimitive?.content

private fun JsonObject.optionalInt(name: String): Int? =
    this[name]?.takeIf { it !is JsonNull }?.jsonPrimitive?.int
