// Golden trace driver for model/CjkText.kt, which compiles VERBATIM from the app tree.
//
// The file has no imports at all: three private ranges, one predicate and one constant. The
// retry-guard prompt is part of the contract (it is appended to a retry request and never
// persisted), so every trace records it.
package cjk.text.golden

import com.newoether.agora.model.CJK_RETRY_GUARD_PROMPT
import com.newoether.agora.model.containsCjk
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
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

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject

    fun text(name: String): String? =
        (root[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

    val name = text("name") ?: ""
    val op = text("op") ?: error("scenario field `op` is required")
    val input = text("text") ?: error("scenario field `text` is required")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("constants|").append(esc(CJK_RETRY_GUARD_PROMPT)).append('\n')
    out.append("args|").append(esc(input)).append('\n')
    out.append("out|bool|").append(input.containsCjk()).append('\n')
    print(out)
}
