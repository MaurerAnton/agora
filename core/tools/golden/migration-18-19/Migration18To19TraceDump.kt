// Golden trace driver for data/local/migration/Migration18To19.kt (compiled verbatim).
//
// The driver provides an in-memory SupportSQLiteDatabase that answers the two projection queries
// the migration issues and records every query/execSQL with typed bind arguments. The trace pins
// the ordered repair UPDATE statements plus any exception class/message.
package migration.eighteen.nineteen.golden

import android.database.Cursor
import androidx.sqlite.db.SupportSQLiteDatabase
import com.newoether.agora.data.local.migration.MIGRATION_18_19
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
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

private fun JsonObject.int(name: String): Int =
    (this[name] as? JsonPrimitive)?.intOrNull ?: 0

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

private data class RunRow(
    val id: String,
    val parentRunId: String?,
    val legacyAmbiguous: Int,
)

private data class MessageRow(
    val id: String,
    val parentId: String?,
    val participant: String,
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

    override fun getInt(columnIndex: Int): Int = value(columnIndex) as Int

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
            sql.startsWith("SELECT id, parentRunId, legacyAmbiguous FROM runs") -> ListCursor(
                listOf("id", "parentRunId", "legacyAmbiguous"),
                runs.map { listOf<Any?>(it.id, it.parentRunId, it.legacyAmbiguous) },
            )
            sql.startsWith("SELECT id, parentId, participant, runId, runSequence FROM messages") ->
                ListCursor(
                    listOf("id", "parentId", "participant", "runId", "runSequence"),
                    messages.map {
                        listOf<Any?>(it.id, it.parentId, it.participant, it.runId, it.runSequence)
                    },
                )
            else -> error("unexpected query: $sql")
        }
    }
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val runs = root.objects("runs").map { item ->
        RunRow(
            id = item.requiredText("id"),
            parentRunId = item.text("parentRunId"),
            legacyAmbiguous = item.int("legacyAmbiguous"),
        )
    }
    val messages = root.objects("messages").map { item ->
        MessageRow(
            id = item.requiredText("id"),
            parentId = item.text("parentId"),
            participant = item.requiredText("participant"),
            runId = item.requiredText("runId"),
            runSequence = item.long("runSequence"),
        )
    }

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("args|").append(runs.size).append('|').append(messages.size).append('\n')

    when (op) {
        "migrate" -> {
            val database = RecordingDatabase(runs, messages)
            val error = runCatching { MIGRATION_18_19.migrate(database) }.exceptionOrNull()
            val callsJson = Json.encodeToString(database.calls)
            val errorText = error?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "-"
            out.append("result|").append(esc(callsJson)).append('|').append(esc(errorText)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
