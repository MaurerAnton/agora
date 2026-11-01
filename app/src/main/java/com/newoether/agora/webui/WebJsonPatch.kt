package com.newoether.agora.webui

import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

/**
 * Structural patch from one encoded streaming frame to the next, applied by `applyStreamingPatch`
 * in assets/webui/sync.js. A streamed reply mostly grows at the tail of its strings and lists, so
 * the browser receives the appended part instead of the whole message on every snapshot.
 *
 * Patch nodes:
 * - `{"v": value}` replaces the value;
 * - `{"a": "suffix"}` appends to a string;
 * - `{"o": {key: patch}, "d": [key]}` patches object members and deletes absent ones;
 * - `{"l": size, "i": {"index": patch}}` resizes a list and patches the listed entries.
 *
 * Returns null when [next] equals [previous].
 */
internal fun webJsonPatch(previous: JsonElement, next: JsonElement): JsonObject? = when {
    previous is JsonObject && next is JsonObject -> objectPatch(previous, next)
    previous is JsonArray && next is JsonArray -> arrayPatch(previous, next)
    previous is JsonPrimitive && next is JsonPrimitive -> primitivePatch(previous, next)
    else -> replace(next)
}

private fun objectPatch(previous: JsonObject, next: JsonObject): JsonObject? {
    val members = buildMap {
        next.forEach { (key, value) ->
            val old = previous[key]
            val patch = if (old == null) replace(value) else webJsonPatch(old, value)
            if (patch != null) put(key, patch)
        }
    }
    val deleted = previous.keys.filterNot(next::containsKey)
    if (members.isEmpty() && deleted.isEmpty()) return null
    return buildJsonObject {
        if (members.isNotEmpty()) put("o", JsonObject(members))
        if (deleted.isNotEmpty()) put("d", buildJsonArray { deleted.forEach { add(JsonPrimitive(it)) } })
    }
}

private fun arrayPatch(previous: JsonArray, next: JsonArray): JsonObject? {
    val entries = buildMap {
        next.forEachIndexed { index, value ->
            val patch = previous.getOrNull(index)?.let { webJsonPatch(it, value) } ?: if (index < previous.size) null else replace(value)
            if (patch != null) put(index.toString(), patch)
        }
    }
    if (entries.isEmpty() && previous.size == next.size) return null
    return buildJsonObject {
        put("l", next.size)
        if (entries.isNotEmpty()) put("i", JsonObject(entries))
    }
}

private fun primitivePatch(previous: JsonPrimitive, next: JsonPrimitive): JsonObject? {
    if (previous == next) return null
    if (previous.isString && next.isString && next.content.length > previous.content.length &&
        next.content.startsWith(previous.content)
    ) {
        return buildJsonObject { put("a", next.content.substring(previous.content.length)) }
    }
    return replace(next)
}

private fun replace(value: JsonElement): JsonObject = JsonObject(mapOf("v" to value))
