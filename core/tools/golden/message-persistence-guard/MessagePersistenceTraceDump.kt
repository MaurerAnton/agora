// Golden trace driver for model/MessagePersistenceGuard.kt.
//
// MessagePersistenceGuard.kt compiles verbatim from the app tree. MessageSegment comes from the
// real ChatMessage.kt through the backup-media-policy extractor; the driver uses the real
// kotlinx serializer to build the scenario input, so the guard is exercised on exactly the model
// the app persists.
package message.guard.golden

import com.newoether.agora.model.MessagePersistenceGuard
import com.newoether.agora.model.MessageSegment
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import java.io.File

private val json = Json

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

private fun JsonObject.str(name: String): String? {
    val element = this[name]
    if (element == null || element is JsonNull) return null
    return (element as? JsonPrimitive)?.content
}

private fun JsonObject.requiredStr(name: String): String =
    str(name) ?: error("scenario field `$name` is required")

private fun JsonObject.int(name: String): Int? =
    (this[name] as? JsonPrimitive)?.let { if (it is JsonNull) null else it.intOrNull }

private fun JsonObject.segments(): List<MessageSegment> {
    val element = this["segments_json"]
    if (element == null || element is JsonNull) return emptyList()
    val array = element as? JsonArray ?: error("segments_json must be an array or null")
    return json.decodeFromString(array.toString())
}

fun main(args: Array<String>) {
    val scenario = File(args[0])
    val root = Json.parseToJsonElement(scenario.readText()).jsonObject
    val name = root.str("name") ?: scenario.nameWithoutExtension
    val fn = root.requiredStr("fn")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("fn|").append(esc(fn)).append('\n')

    when (fn) {
        "clipText" -> {
            val text = root.str("text") ?: ""
            val result = MessagePersistenceGuard.clipText(text)
            out.append("inputUtf16|").append(text.length).append('\n')
            out.append("inputUtf8|").append(text.toByteArray(Charsets.UTF_8).size).append('\n')
            out.append("resultUtf16|").append(result.length).append('\n')
            out.append("resultUtf8|").append(result.toByteArray(Charsets.UTF_8).size).append('\n')
            out.append("result|").append(esc(result)).append('\n')
        }

        "encodeSegmentsBounded" -> {
            val segments = root.segments()
            val canonical = json.encodeToString(segments)
            out.append("canonical|").append(esc(canonical)).append('\n')
            out.append("canonicalUtf8|")
                .append(canonical.toByteArray(Charsets.UTF_8).size).append('\n')
            val maxBytes = root.int("maxBytes")
            try {
                val result = if (maxBytes == null) {
                    MessagePersistenceGuard.encodeSegmentsBounded(segments)
                } else {
                    MessagePersistenceGuard.encodeSegmentsBounded(segments, maxBytes)
                }
                if (result == null) {
                    out.append("result|null\n")
                } else {
                    out.append("resultUtf16|").append(result.length).append('\n')
                    out.append("resultUtf8|")
                        .append(result.toByteArray(Charsets.UTF_8).size).append('\n')
                    out.append("result|").append(esc(result)).append('\n')
                }
            } catch (error: Throwable) {
                out.append("error|").append(error::class.simpleName).append('|')
                    .append(esc(error.message.orEmpty())).append('\n')
            }
        }

        else -> error("unknown function `$fn`")
    }
    print(out)
}
