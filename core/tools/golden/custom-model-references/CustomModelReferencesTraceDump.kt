// Golden trace driver for data/CustomModelReferences.kt, which compiles VERBATIM from the app tree.
//
// The file has no imports at all: three extension functions over containers. Everything interesting
// is the container behaviour, so the driver feeds real `LinkedHashSet` / `LinkedHashMap` instances
// (the iteration order the callers see) and prints the result in iteration order.
package custom.model.references.golden

import com.newoether.agora.data.replaceCustomModelAlias
import com.newoether.agora.data.replaceModelReference
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import java.io.File

/** Field-safe escaping: no raw separators or control characters survive. '=' is escaped too so
 * the map's `key=>value` pairs stay unambiguous even when an id contains an '='. */
private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '=' -> out.append("\\e")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

private fun opt(value: String?): String = value?.let(::esc) ?: "-"

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject

    fun text(name: String): String? =
        (root[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

    fun strings(name: String): List<String> =
        (root[name] as JsonArray).map { (it as JsonPrimitive).content }

    fun entries(name: String): List<Pair<String, String>> =
        (root[name] as JsonArray).map {
            val pair = it as JsonArray
            (pair[0] as JsonPrimitive).content to (pair[1] as JsonPrimitive).content
        }

    val op = text("op") ?: error("scenario field `op` is required")
    val oldModelId = text("oldModelId") ?: error("scenario field `oldModelId` is required")
    val newModelId = text("newModelId")

    val out = StringBuilder()
    out.append("name|").append(esc(text("name") ?: "")).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "set" -> {
            val items = strings("items")
            val input = linkedSetOf<String>().apply { addAll(items) }
            val result = input.replaceModelReference(oldModelId, newModelId)
            out.append("args|").append(esc(oldModelId)).append('|').append(opt(newModelId))
                .append('|').append(items.joinToString("|") { esc(it) }).append('\n')
            out.append("out|set|").append(result.joinToString("|") { esc(it) }).append('\n')
        }

        "string" -> {
            val value = text("value")
            val result = value.replaceModelReference(oldModelId, newModelId)
            out.append("args|").append(opt(value)).append('|').append(esc(oldModelId)).append('|')
                .append(opt(newModelId)).append('\n')
            out.append("out|string?|").append(opt(result)).append('\n')
        }

        "map" -> {
            val alias = text("alias") ?: error("scenario field `alias` is required")
            val input = linkedMapOf<String, String>().apply {
                entries("entries").forEach { (key, value) -> put(key, value) }
            }
            val result = input.replaceCustomModelAlias(oldModelId, newModelId, alias)
            out.append("args|").append(esc(oldModelId)).append('|').append(opt(newModelId))
                .append('|').append(esc(alias)).append('|')
                .append(entries("entries").joinToString("|") { (key, value) ->
                    esc(key) + "=>" + esc(value)
                }).append('\n')
            out.append("out|map|")
                .append(result.entries.joinToString("|") { (key, value) ->
                    esc(key) + "=>" + esc(value)
                }).append('\n')
        }

        else -> error("unknown op: $op")
    }

    print(out)
}
