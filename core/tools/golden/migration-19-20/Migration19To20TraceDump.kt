// Golden trace driver for data/local/migration/Migration19To20.kt (compiled verbatim).
//
// The migration is SQL-only, so the driver just records every execSQL call with typed bind
// arguments and the exception text. The trace pins the exact three repair statements, their
// order, and the Kotlin runtime types of the bind arguments (Int limits, String marker).
package migration.nineteen.twenty.golden

import androidx.sqlite.db.SupportSQLiteDatabase
import com.newoether.agora.data.local.migration.MIGRATION_19_20
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
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

private fun renderArg(value: Any?): String = when (value) {
    null -> "null"
    is String -> "s:$value"
    is Long -> "l:$value"
    is Int -> "i:$value"
    else -> "o:${value.javaClass.simpleName}:$value"
}

private class RecordingDatabase : SupportSQLiteDatabase {
    val calls = mutableListOf<List<String>>()

    override fun execSQL(sql: String) {
        execSQL(sql, emptyArray<Any?>())
    }

    override fun execSQL(sql: String, bindArgs: Array<out Any?>) {
        calls += listOf("execSQL", sql) + bindArgs.map(::renderArg)
    }
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("args|0|0\n")

    when (op) {
        "migrate" -> {
            val database = RecordingDatabase()
            val error = runCatching { MIGRATION_19_20.migrate(database) }.exceptionOrNull()
            val callsJson = Json.encodeToString(database.calls)
            val errorText = error?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "-"
            out.append("result|").append(esc(callsJson)).append('|').append(esc(errorText)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
