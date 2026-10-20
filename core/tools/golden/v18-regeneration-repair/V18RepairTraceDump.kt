// Golden trace driver for data/local/migration/V18RegenerationEdgeRepairPlanner.kt
// (compiled verbatim from the app tree, together with the real util/Constants.kt).
//
// `plan` prints the ordered repair map as `id=parent` joined by `;`. Order matters: the planner
// returns a LinkedHashMap whose insertion order is part of the migration's emitted updates.
// `constants` pins the prefixes the planner's ordinary-message filter depends on.
package v18.regeneration.repair.golden

import com.newoether.agora.data.local.migration.V18MessageRecord
import com.newoether.agora.data.local.migration.V18RegenerationEdgeRepairPlanner
import com.newoether.agora.data.local.migration.V18RunRecord
import com.newoether.agora.model.Participant
import com.newoether.agora.util.Constants
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull
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

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private fun JsonObject.long(name: String): Long =
    (this[name] as? JsonPrimitive)?.longOrNull ?: 0L

private fun JsonObject.boolean(name: String, fallback: Boolean): Boolean =
    (this[name] as? JsonPrimitive)?.booleanOrNull ?: fallback

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "plan" -> {
            val runs = root.objects("runs").map { item ->
                V18RunRecord(
                    id = item.requiredText("id"),
                    parentRunId = item.text("parentRunId"),
                    legacyAmbiguous = item.boolean("legacyAmbiguous", false),
                )
            }
            val messages = root.objects("messages").map { item ->
                V18MessageRecord(
                    id = item.requiredText("id"),
                    parentId = item.text("parentId"),
                    participant = Participant.valueOf(item.requiredText("participant")),
                    runId = item.requiredText("runId"),
                    runSequence = item.long("runSequence"),
                )
            }
            val updates = V18RegenerationEdgeRepairPlanner.plan(runs, messages)
            out.append("args|").append(runs.size).append('|').append(messages.size).append('\n')
            out.append("result|").append(esc(
                updates.entries.joinToString(";") { "${it.key}=${it.value}" }
            )).append('\n')
        }
        "constants" -> {
            out.append("args|\n")
            out.append("result|")
                .append(esc(Constants.TOOL_MSG_PREFIX)).append('|')
                .append(esc(Constants.RESULT_MSG_PREFIX)).append('|')
                .append(esc(Constants.COMPACT_MSG_PREFIX)).append('|')
                .append(esc(Constants.TOOL_CALL_ID_PREFIX)).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
