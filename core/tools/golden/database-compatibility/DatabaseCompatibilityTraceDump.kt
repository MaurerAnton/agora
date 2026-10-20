// Golden trace driver for data/local/DatabaseCompatibility.kt (compiled verbatim).
//
// `classify` prints the sealed-interface toString(); `can_open` prints the canOpen property. The
// stored-version reader is simulated: a scenario either returns storedVersion or throws the named
// JDK exception with the given message.
package database.compatibility.golden

import com.newoether.agora.data.local.DatabaseCompatibility
import com.newoether.agora.data.local.canOpen
import com.newoether.agora.data.local.classifyDatabaseCompatibility
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
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

private fun JsonObject.integer(name: String): Int? =
    (this[name] as? JsonPrimitive)?.intOrNull

private fun JsonObject.boolean(name: String, fallback: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.booleanOrNull ?: fallback

private fun opt(value: Any?): String = value?.let { esc(it.toString()) } ?: "-"

private fun buildThrowable(className: String, message: String?): Exception = when (className) {
    "java.lang.IllegalStateException" -> IllegalStateException(message)
    "java.lang.NullPointerException" -> NullPointerException(message)
    "java.lang.IllegalArgumentException" -> IllegalArgumentException(message)
    "java.lang.RuntimeException" -> RuntimeException(message)
    "java.io.IOException" -> java.io.IOException(message)
    else -> error("unknown throwable $className")
}

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    if (op == "classify" || op == "can_open") {
        val exists = root.boolean("databaseExists", false)
        val currentVersion = root.integer("currentVersion") ?: 0
        val storedVersion = root.integer("storedVersion")
        val throwClass = root.text("throwClass")
        val throwMessage = root.text("throwMessage")

        val readStoredVersion: () -> Int = if (throwClass != null) {
            { throw buildThrowable(throwClass, throwMessage) }
        } else {
            { storedVersion ?: 0 }
        }

        val result = classifyDatabaseCompatibility(exists, currentVersion, readStoredVersion)
        out.append("args|")
            .append(exists).append('|')
            .append(currentVersion).append('|')
            .append(opt(storedVersion)).append('|')
            .append(opt(throwClass)).append('|')
            .append(opt(throwMessage)).append('\n')
        out.append("result|")
        if (op == "classify") {
            out.append(esc(result.toString()))
        } else {
            out.append(result.canOpen)
        }
        out.append('\n')
    } else {
        error("unknown op: $op")
    }
    print(out)
}
