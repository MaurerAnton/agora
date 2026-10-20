// Golden trace driver for data/local/migration/Migration16To17.kt (compiled verbatim).
//
// The driver provides an in-memory SupportSQLiteDatabase that answers exactly the three query
// shapes the migration issues and records every execSQL/query with typed bind arguments. The trace
// pins the complete SQL surface (DDL order, INSERT/UPDATE arguments, selection JSON) plus any
// exception class/message raised after the recorded prefix.
package migration.sixteen.seventeen.golden

import android.database.Cursor
import androidx.sqlite.db.SupportSQLiteDatabase
import com.newoether.agora.data.local.migration.MIGRATION_16_17
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

private data class ConversationRow(val id: String, val selectedBranchesJson: String?)

private data class MessageRow(
    val conversationId: String,
    val id: String,
    val parentId: String?,
    val participant: String,
    val status: String,
    val timestamp: Long,
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
    private val messages: List<MessageRow>,
) : SupportSQLiteDatabase {
    val calls = mutableListOf<List<String>>()
    private val assigned = mutableSetOf<String>()

    private fun record(kind: String, sql: String, args: List<Any?>) {
        calls += listOf(kind, sql) + args.map(::renderArg)
    }

    override fun execSQL(sql: String) {
        execSQL(sql, emptyArray<Any?>())
    }

    override fun execSQL(sql: String, bindArgs: Array<out Any?>) {
        record("execSQL", sql, bindArgs.toList())
        if (sql.startsWith("UPDATE messages")) {
            assigned += bindArgs[3] as String
        }
    }

    override fun query(sql: String): Cursor = query(sql, emptyArray<Any?>())

    override fun query(sql: String, bindArgs: Array<out Any?>?): Cursor {
        val args = bindArgs?.toList() ?: emptyList()
        record("query", sql, args)
        return when {
            sql.startsWith("SELECT id, selectedBranchesJson FROM conversations") -> ListCursor(
                listOf("id", "selectedBranchesJson"),
                conversations.map { listOf<Any?>(it.id, it.selectedBranchesJson) },
            )
            sql.startsWith("SELECT id, parentId, participant, status, timestamp") -> {
                val conversationId = args[0] as String
                ListCursor(
                    listOf("id", "parentId", "participant", "status", "timestamp"),
                    messages.filter { it.conversationId == conversationId }.map {
                        listOf<Any?>(it.id, it.parentId, it.participant, it.status, it.timestamp)
                    },
                )
            }
            sql.startsWith("SELECT COUNT(*) FROM messages WHERE runId IS NULL") -> ListCursor(
                listOf("COUNT(*)"),
                listOf(listOf<Any?>(messages.count { it.id !in assigned }.toLong())),
            )
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
        )
    }
    val messages = root.objects("messages").map { item ->
        MessageRow(
            conversationId = item.requiredText("conversationId"),
            id = item.requiredText("id"),
            parentId = item.text("parentId"),
            participant = item.requiredText("participant"),
            status = item.requiredText("status"),
            timestamp = item.long("timestamp"),
        )
    }

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("args|").append(conversations.size).append('|').append(messages.size).append('\n')

    when (op) {
        "migrate" -> {
            val database = RecordingDatabase(conversations, messages)
            val error = runCatching { MIGRATION_16_17.migrate(database) }.exceptionOrNull()
            val callsJson = Json.encodeToString(database.calls)
            val errorText = error?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "-"
            out.append("result|").append(esc(callsJson)).append('|').append(esc(errorText)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
