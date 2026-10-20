// Golden trace driver for model/OpenAiServiceTiers.kt (compiled verbatim from the app tree).
//
// One op per public function: `normalize`, `available`, `mapped`, `index`, `tier`, `request`.
// Null strings print as `-`, mirroring the other stands; booleans print as true/false.
package openai.service.tiers.golden

import com.newoether.agora.model.OpenAiServiceTiers
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

private fun JsonObject.bool(name: String, default: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.booleanOrNull ?: default

private fun JsonObject.integer(name: String): Int? =
    (this[name] as? JsonPrimitive)?.intOrNull

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private fun opt(value: String?): String = value?.let { esc(it) } ?: "-"

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "normalize" -> {
            val value = root.text("value")
            out.append("args|").append(opt(value)).append('\n')
            out.append("result|").append(esc(OpenAiServiceTiers.normalize(value))).append('\n')
        }
        "available" -> {
            val model = root.text("model")
            val official = root.bool("official", true)
            out.append("args|").append(opt(model)).append('|').append(official).append('\n')
            out.append("result|")
                .append(esc(OpenAiServiceTiers.availableTiers(model, official).joinToString(",")))
                .append('\n')
        }
        "mapped" -> {
            val value = root.text("value")
            val model = root.text("model")
            val official = root.bool("official", true)
            out.append("args|").append(opt(value)).append('|').append(opt(model)).append('|')
                .append(official).append('\n')
            out.append("result|")
                .append(esc(OpenAiServiceTiers.mappedTier(value, model, official)))
                .append('\n')
        }
        "index" -> {
            val value = root.text("value")
            out.append("args|").append(opt(value)).append('\n')
            out.append("result|").append(OpenAiServiceTiers.indexForTier(value)).append('\n')
        }
        "tier" -> {
            val index = root.integer("index") ?: 0
            out.append("args|").append(index).append('\n')
            out.append("result|").append(esc(OpenAiServiceTiers.tierForIndex(index))).append('\n')
        }
        "request" -> {
            val enabled = root.bool("enabled", false)
            val value = root.text("value")
            val responses = root.bool("responses", false)
            val model = root.text("model")
            val official = root.bool("official", true)
            out.append("args|").append(enabled).append('|').append(opt(value)).append('|')
                .append(responses).append('|').append(opt(model)).append('|').append(official)
                .append('\n')
            val result = OpenAiServiceTiers.requestValue(
                enabled = enabled,
                value = value,
                responsesApiEnabled = responses,
                modelId = model,
                officialProvider = official,
            )
            out.append("result|").append(opt(result)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
