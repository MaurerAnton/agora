// Golden trace driver for model/MessageSource.kt (compiled verbatim from the app tree).
//
// Ops: `decode` (stored JSON -> data class), `encode` (stored JSON -> canonical JSON),
// `sanitize` (import gate), `readable` (ask_user text), `automation` (auto prompt source),
// `constants`. Null strings print as `-`; results are Kotlin toString/JSON strings.
package message.source.golden

import com.newoether.agora.model.MessageSource
import com.newoether.agora.model.Participant
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
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

private fun JsonObject.items(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

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
        "decode" -> {
            val raw = root.text("raw")
            out.append("args|").append(opt(raw)).append('\n')
            out.append("result|").append(opt(MessageSource.decode(raw)?.toString())).append('\n')
        }
        "encode" -> {
            val raw = root.text("raw")
            out.append("args|").append(opt(raw)).append('\n')
            out.append("result|").append(opt(MessageSource.encode(MessageSource.decode(raw)))).append('\n')
        }
        "sanitize" -> {
            val raw = root.text("raw")
            val participant = Participant.valueOf(root.requiredText("participant"))
            out.append("args|").append(opt(raw)).append('|').append(participant.name).append('\n')
            out.append("result|")
                .append(opt(MessageSource.sanitizeImported(raw, participant)))
                .append('\n')
        }
        "readable" -> {
            val items = root.items("items").map { item ->
                MessageSource.AskUserItem(
                    question = item.requiredText("question"),
                    answer = item.text("answer"),
                )
            }
            out.append("args|").append(items.size)
            for (item in items) {
                out.append('|').append(esc(item.question)).append('|').append(opt(item.answer))
            }
            out.append('\n')
            out.append("result|")
                .append(esc(MessageSource.askUser(items).askUserReadableText()))
                .append('\n')
        }
        "automation" -> {
            val kind = root.text("requestKind")
            out.append("args|").append(opt(kind)).append('\n')
            out.append("result|")
                .append(opt(MessageSource.forAutomationRequestKind(kind.orEmpty())?.toString()))
                .append('\n')
        }
        "constants" -> {
            out.append("args|\n")
            out.append("result|")
                .append(esc(MessageSource.NO_ANSWER))
                .append('|')
                .append(esc(MessageSource.TASK.toString()))
                .append('|')
                .append(esc(MessageSource.LOOP.toString()))
                .append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
