// Golden trace driver for the streaming-json-document cluster:
//
//   ui/chat/message/StreamingJsonDocument.kt      prefix JSON parser (data classes + parser)
//   ui/chat/message/StreamingToolArgumentHints.kt bounded hint extraction + subject normalization
//   ui/chat/message/ToolPresentation.kt           ToolKind enum only (extracted by script)
//
// StreamingJsonDocument.kt and StreamingToolArgumentHints.kt compile VERBATIM from the app tree
// (zero imports); ToolKind is lifted declaration-for-declaration by extract_tool_kind.py because
// ToolPresentation.kt drags kotlinx.serialization and ChatMessage along.
//
// The document and hint results are printed with Kotlin's data-class `toString()`, so every node,
// complete flag, entry key and error offset is pinned. The parser indexes UTF-16 code units —
// errorOffset is a UI cursor — and the traces include non-ASCII before an error to keep that
// honest.
package streaming.json.golden

import com.newoether.agora.ui.chat.message.StreamingJsonParser
import com.newoether.agora.ui.chat.message.StreamingToolArgumentHintResolver
import com.newoether.agora.ui.chat.message.ToolKind
import com.newoether.agora.ui.chat.message.normalizeToolSummarySubject
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.longOrNull
import java.io.File

/** Field-safe escaping: no raw separators or control characters survive. */
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

private fun JsonObject.number(name: String): Long? =
    (this[name] as? JsonPrimitive)?.longOrNull

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
        "parse" -> {
            val source = root.requiredText("source")
            out.append("args|").append(esc(source)).append('\n')
            out.append("result|")
                .append(esc(StreamingJsonParser.parse(source).toString()))
                .append('\n')
        }
        "hints" -> {
            val kind = ToolKind.valueOf(root.requiredText("kind"))
            val raw = root.text("rawArguments")
            out.append("args|").append(esc(kind.name)).append('|').append(opt(raw)).append('\n')
            out.append("result|")
                .append(esc(StreamingToolArgumentHintResolver.resolve(kind, raw).toString()))
                .append('\n')
        }
        "normalize" -> {
            val value = root.text("value")
            val max = root.number("max")?.toInt() ?: 120
            out.append("args|").append(opt(value)).append('|').append(max).append('\n')
            val normalized = normalizeToolSummarySubject(value, max)
            out.append("result|").append(normalized?.let { esc(it) } ?: "null").append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
