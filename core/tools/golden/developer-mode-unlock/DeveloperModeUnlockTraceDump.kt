// Golden trace driver for ui/settings/DeveloperModeUnlockPolicy.kt (compiled verbatim).
//
// `advance` pins the tap counter, the coerceIn clamp and the remaining-taps feedback threshold;
// `constants` pins REQUIRED_TAPS. Results print as the Kotlin data-class toString().
package developer.mode.unlock.golden

import com.newoether.agora.ui.settings.DeveloperModeUnlockPolicy
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

private fun JsonObject.integer(name: String): Int? =
    (this[name] as? JsonPrimitive)?.intOrNull

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "advance" -> {
            val current = root.integer("currentTapCount") ?: 0
            val enabled = (root["alreadyEnabled"] as? JsonPrimitive)?.booleanOrNull ?: false
            out.append("args|").append(current).append('|').append(enabled).append('\n')
            out.append("result|")
                .append(esc(DeveloperModeUnlockPolicy.advance(current, enabled).toString()))
                .append('\n')
        }
        "constants" -> {
            out.append("args|\n")
            out.append("result|").append(DeveloperModeUnlockPolicy.REQUIRED_TAPS).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
