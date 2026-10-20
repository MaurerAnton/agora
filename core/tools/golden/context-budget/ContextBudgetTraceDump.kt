// Golden trace driver for model/ContextBudget.kt (compiled verbatim from the app tree).
//
// Two ops mirror the public surface: `normalize` (legacy-setting migration + clamp) and `label`
// (compact occupancy label). Null values print as `-`, mirroring the other stands.
package context.budget.golden

import com.newoether.agora.model.ContextBudget
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

private fun JsonObject.integer(name: String): Int? =
    (this[name] as? JsonPrimitive)?.intOrNull

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private fun opt(value: Int?): String = value?.toString() ?: "-"

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "normalize" -> {
            val value = root.integer("value")
            out.append("args|").append(opt(value)).append('\n')
            out.append("result|").append(ContextBudget.normalize(value)).append('\n')
        }
        "label" -> {
            val tokens = root.integer("tokens") ?: 0
            out.append("args|").append(tokens).append('\n')
            out.append("result|").append(esc(ContextBudget.compactLabel(tokens))).append('\n')
        }
        "presets" -> {
            out.append("args|\n")
            out.append("result|")
                .append(esc(ContextBudget.PRESETS.joinToString(",")))
                .append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
