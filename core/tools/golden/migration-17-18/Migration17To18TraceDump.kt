// Golden trace driver for data/local/migration/Migration17To18.kt (compiled verbatim).
//
// The driver provides an in-memory SupportSQLiteDatabase that answers exactly the three query
// shapes the migration issues and records every query/execSQL with typed bind arguments. The trace
// pins the complete SQL surface (repair updates in planner order, selection JSON, deletions) plus
// any exception class/message raised after the recorded prefix.
package migration.seventeen.eighteen.golden

import android.database.Cursor
import androidx.sqlite.db.SupportSQLiteDatabase
import com.newoether.agora.data.local.migration.MIGRATION_17_18
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.io.File

private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

private fun JsonObject.text(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private fun JsonObject.long(name: String): Long =
    (this[name] as? JsonPrimitive)?.longOrNull ?: 0L

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

private data class ConversationRow(
    val id: String,
    val selectedBranchesJson: String?,
    val selectedRunBranchesJson: String?,
)

private data class RunRow(
    val conversationId: String,
    val id: String,
    val parentRunId: String?,
    val startedAt: Long,
)

private data class MessageRow(
    val conversationId: String,
    val id: String,
    val parentId: String?,
    val text: String,
    val images: String,
    val attachmentMeta: String?,
    val participant: String,
    val timestamp: Long,
    val runId: String,
    val runSequence: Long,
)

private class ListCursor(
    private val columns: List<String>,
    private val rows: List<List<Any?>>,
) : Cursor {
    private var position = -1

    override fun getColumnIndexOrThrow(columnName: String): Int {
        val index = columns.indexOf(columnName)
        require(index >= 0) { "column '$columnName' does not exist" }
        return index
    }

    override fun moveToFirst(): Boolean {
        position = 0
        return rows.isNotEmpty()
    }

    override fun moveToNext(): Boolean {
        position += 1
        return position < rows.size
    }

    private fun value(columnIndex: Int): Any? = rows[position][columnIndex]

    override fun getString(columnIndex: Int): String = value(columnIndex) as String

    override fun isNull(columnIndex: Int): Boolean = value(columnIndex) == null

    override fun getLong(columnIndex: Int): Long = value(columnIndex) as Long

    override fun close() = Unit
}

private fun renderArg(value: Any?): String = when (value) {
    null -> "null"
    is String -> "s:$value"
    is Long -> "l:$value"
    is Int -> "i:$value"
    else -> "o:${value.javaClass.simpleName}:$value"
}

private class RecordingDatabase(
    private val conversations: List<ConversationRow>,
    private val runs: List<RunRow>,
    private val messages: List<MessageRow>,
) : SupportSQLiteDatabase {
    val calls = mutableListOf<List<String>>()

    private fun record(kind: String, sql: String, args: List<Any?>) {
        calls += listOf(kind, sql) + args.map(::renderArg)
    }

    override fun execSQL(sql: String) {
        execSQL(sql, emptyArray<Any?>())
    }

    override fun execSQL(sql: String, bindArgs: Array<out Any?>) {
        record("execSQL", sql, bindArgs.toList())
    }

    override fun query(sql: String): Cursor = query(sql, emptyArray<Any?>())

    override fun query(sql: String, bindArgs: Array<out Any?>?): Cursor {
        val args = bindArgs?.toList() ?: emptyList()
        record("query", sql, args)
        return when {
            sql.startsWith("SELECT id, selectedBranchesJson, selectedRunBranchesJson FROM conversations") ->
                ListCursor(
                    listOf("id", "selectedBranchesJson", "selectedRunBranchesJson"),
                    conversations.map {
                        listOf<Any?>(it.id, it.selectedBranchesJson, it.selectedRunBranchesJson)
                    },
                )
            sql.startsWith("SELECT id, parentRunId, startedAt") -> {
                val conversationId = args[0] as String
                ListCursor(
                    listOf("id", "parentRunId", "startedAt"),
                    runs.filter { it.conversationId == conversationId }.map {
                        listOf<Any?>(it.id, it.parentRunId, it.startedAt)
                    },
                )
            }
            sql.startsWith(
                "SELECT id, parentId, text, images, attachmentMeta, participant, timestamp, runId, runSequence",
            ) -> {
                val conversationId = args[0] as String
                ListCursor(
                    listOf(
                        "id", "parentId", "text", "images", "attachmentMeta", "participant",
                        "timestamp", "runId", "runSequence",
                    ),
                    messages.filter { it.conversationId == conversationId }.map {
                        listOf<Any?>(
                            it.id, it.parentId, it.text, it.images, it.attachmentMeta,
                            it.participant, it.timestamp, it.runId, it.runSequence,
                        )
                    },
                )
            }
            else -> error("unexpected query: $sql")
        }
    }
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val conversations = root.objects("conversations").map { item ->
        ConversationRow(
            id = item.requiredText("id"),
            selectedBranchesJson = item.text("selectedBranchesJson"),
            selectedRunBranchesJson = item.text("selectedRunBranchesJson"),
        )
    }
    val runs = root.objects("runs").map { item ->
        RunRow(
            conversationId = item.requiredText("conversationId"),
            id = item.requiredText("id"),
            parentRunId = item.text("parentRunId"),
            startedAt = item.long("startedAt"),
        )
    }
    val messages = root.objects("messages").map { item ->
        MessageRow(
            conversationId = item.requiredText("conversationId"),
            id = item.requiredText("id"),
            parentId = item.text("parentId"),
            text = item.text("text") ?: "",
            images = item.text("images") ?: "",
            attachmentMeta = item.text("attachmentMeta"),
            participant = item.requiredText("participant"),
            timestamp = item.long("timestamp"),
            runId = item.requiredText("runId"),
            runSequence = item.long("runSequence"),
        )
    }

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("args|").append(conversations.size).append('|').append(runs.size)
        .append('|').append(messages.size).append('\n')

    when (op) {
        "migrate" -> {
            val database = RecordingDatabase(conversations, runs, messages)
            val error = runCatching { MIGRATION_17_18.migrate(database) }.exceptionOrNull()
            val callsJson = Json.encodeToString(database.calls)
            val errorText = error?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "-"
            out.append("result|").append(esc(callsJson)).append('|').append(esc(errorText)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
