// Golden trace driver for the v20..v35 SQL-only schema migrations (compiled verbatim).
//
// The scenario names one source version; the driver runs the matching Room Migration and records
// every execSQL call with typed bind arguments plus any exception text. The trace pins each
// statement, its order and its whitespace.
package migration.twenty.thirtyfive.golden

import androidx.room.migration.Migration
import androidx.sqlite.db.SupportSQLiteDatabase
import com.newoether.agora.data.local.migration.MIGRATION_20_21
import com.newoether.agora.data.local.migration.MIGRATION_21_22
import com.newoether.agora.data.local.migration.MIGRATION_22_23
import com.newoether.agora.data.local.migration.MIGRATION_23_24
import com.newoether.agora.data.local.migration.MIGRATION_24_25
import com.newoether.agora.data.local.migration.MIGRATION_25_26
import com.newoether.agora.data.local.migration.MIGRATION_26_27
import com.newoether.agora.data.local.migration.MIGRATION_27_28
import com.newoether.agora.data.local.migration.MIGRATION_28_29
import com.newoether.agora.data.local.migration.MIGRATION_29_30
import com.newoether.agora.data.local.migration.MIGRATION_30_31
import com.newoether.agora.data.local.migration.MIGRATION_31_32
import com.newoether.agora.data.local.migration.MIGRATION_32_33
import com.newoether.agora.data.local.migration.MIGRATION_33_34
import com.newoether.agora.data.local.migration.MIGRATION_34_35
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
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

private fun JsonObject.int(name: String): Int =
    (this[name] as? JsonPrimitive)?.intOrNull ?: error("scenario field `$name` is required")

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

private fun migrationFor(version: Int): Migration = when (version) {
    20 -> MIGRATION_20_21
    21 -> MIGRATION_21_22
    22 -> MIGRATION_22_23
    23 -> MIGRATION_23_24
    24 -> MIGRATION_24_25
    25 -> MIGRATION_25_26
    26 -> MIGRATION_26_27
    27 -> MIGRATION_27_28
    28 -> MIGRATION_28_29
    29 -> MIGRATION_29_30
    30 -> MIGRATION_30_31
    31 -> MIGRATION_31_32
    32 -> MIGRATION_32_33
    33 -> MIGRATION_33_34
    34 -> MIGRATION_34_35
    else -> error("unknown source version: $version")
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")
    val version = root.int("version")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("version|").append(version).append('\n')

    when (op) {
        "migrate" -> {
            val database = RecordingDatabase()
            val error = runCatching { migrationFor(version).migrate(database) }.exceptionOrNull()
            val callsJson = Json.encodeToString(database.calls)
            val errorText = error?.let { "${it.javaClass.simpleName}: ${it.message}" } ?: "-"
            out.append("result|").append(esc(callsJson)).append('|').append(esc(errorText)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
