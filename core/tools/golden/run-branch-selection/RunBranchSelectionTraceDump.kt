// Golden trace driver for data/local/RunBranchSelectionIntegrity.kt, which compiles VERBATIM from
// the app tree.
//
// The policy is declared over RunEntity; the declaration is lifted out verbatim by
// extract_run_entity.py (RunEntity.kt cannot compile host-side). Every Run built here is a terminal
// COMPLETED Run, which satisfies the entity's own `init` invariants; the policy itself reads only
// `id` and `parentRunId`.
package run.branch.selection.golden

import com.newoether.agora.data.local.RunBranchSelectionIntegrity
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
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
            '=' -> out.append("\\e")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

/** `n` for the null key, `s:` + escaped id for a real one: the two can never collide. */
private fun encodeKey(value: String?): String = if (value == null) "n" else "s:" + esc(value)

private fun encodeValue(value: String): String = "s:" + esc(value)

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject

    fun text(name: String): String? =
        (root[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

    val selections = linkedMapOf<String?, String>()
    for (element in root["selections"] as JsonArray) {
        val pair = element as JsonArray
        val key = (pair[0] as? JsonPrimitive)?.takeIf { it.isString }?.content
        val child = (pair[1] as JsonPrimitive).content
        selections[key] = child
    }

    val runs = (root["runs"] as JsonArray).map { element ->
        val fields = element as JsonObject
        fun field(name: String): String? =
            (fields[name] as? JsonPrimitive)?.takeIf { it.isString }?.content
        RunEntity(
            id = field("id") ?: error("run `id` is required"),
            conversationId = "conversation",
            parentRunId = field("parentRunId"),
            status = RunStatus.COMPLETED,
            activeSlot = null,
            startedAt = 1L,
            lastCheckpointAt = 2L,
            endedAt = 3L,
            endReason = RunEndReason.MODEL_COMPLETED,
        )
    }

    val result = RunBranchSelectionIntegrity.retainValidEdges(selections, runs)

    val out = StringBuilder()
    out.append("name|").append(esc(text("name") ?: "")).append('\n')
    out.append("op|retainValidEdges\n")
    out.append("selections|")
        .append(selections.entries.joinToString("|") { (key, child) ->
            encodeKey(key) + "=>" + encodeValue(child)
        })
        .append('\n')
    out.append("runs|")
        .append(runs.joinToString("|") { run ->
            encodeKey(run.id) + "=>" + encodeKey(run.parentRunId)
        })
        .append('\n')
    out.append("out|map|")
        .append(result.entries.joinToString("|") { (key, child) ->
            encodeKey(key) + "=>" + encodeValue(child)
        })
        .append('\n')
    print(out)
}
