// Golden trace driver for data/local/migration/RegenerationTreeRepairPlanner.kt (compiled
// verbatim from the app tree).
//
// `plan` prints the repair plan section by section; map/set insertion order is part of the
// migration's emitted SQL and therefore pinned. `fingerprint` pins the v17 input fingerprint
// (attachment sanitization + SHA-256) used to decide which sibling Runs share an input.
package regeneration.tree.repair.golden

import com.newoether.agora.data.local.migration.RegenerationTreeRepairPlan
import com.newoether.agora.data.local.migration.RegenerationTreeRepairPlanner
import com.newoether.agora.data.local.migration.V17MessageRecord
import com.newoether.agora.data.local.migration.V17RunRecord
import com.newoether.agora.data.local.migration.regenerationInputFingerprint
import com.newoether.agora.model.Participant
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
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

private fun JsonObject.integer(name: String): Int =
    (this[name] as? JsonPrimitive)?.intOrNull ?: 0

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

private fun JsonObject.obj(name: String): JsonObject =
    this[name] as? JsonObject ?: JsonObject(emptyMap())

/** Scenario JSON keys are strings; the literal key `null` is the nullable-map null key. */
private fun JsonObject.selections(): LinkedHashMap<String?, String> {
    val out = LinkedHashMap<String?, String>()
    for ((key, value) in this) {
        val text = (value as? JsonPrimitive)?.takeIf { it.isString }?.content ?: continue
        out[if (key == "null") null else key] = text
    }
    return out
}

private fun pairDesc(map: Map<String, String>): String =
    map.entries.joinToString(";") { "${it.key}=${it.value}" }

private fun seqDesc(map: Map<String, Long>): String =
    map.entries.joinToString(";") { "${it.key}=${it.value}" }

private fun nullableDesc(map: Map<String?, String>): String =
    map.entries.joinToString(";") { "${it.key ?: "<null>"}=${it.value}" }

private fun render(plan: RegenerationTreeRepairPlan): String = listOf(
    "runParent=" + pairDesc(plan.runParentUpdates),
    "deleted=" + plan.deletedMessageIds.joinToString(","),
    "messageParent=" + pairDesc(plan.messageParentUpdates),
    "runSequence=" + seqDesc(plan.runSequenceUpdates),
    "messageSelections=" + nullableDesc(plan.messageSelections),
    "runSelections=" + nullableDesc(plan.runSelections),
    "inferred=" + plan.inferredRunIds.joinToString(","),
).joinToString("|")

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
                V17RunRecord(
                    id = item.requiredText("id"),
                    parentRunId = item.text("parentRunId"),
                    startedAt = item.long("startedAt"),
                )
            }
            val messages = root.objects("messages").map { item ->
                V17MessageRecord(
                    id = item.requiredText("id"),
                    parentId = item.text("parentId"),
                    participant = Participant.valueOf(item.requiredText("participant")),
                    timestamp = item.long("timestamp"),
                    runId = item.requiredText("runId"),
                    runSequence = item.long("runSequence"),
                    inputFingerprint = item.requiredText("inputFingerprint"),
                )
            }
            val plan = RegenerationTreeRepairPlanner.plan(
                runs = runs,
                messages = messages,
                messageSelections = root.obj("messageSelections").selections(),
                runSelections = root.obj("runSelections").selections(),
            )
            out.append("args|").append(runs.size).append('|').append(messages.size).append('\n')
            out.append("result|").append(esc(render(plan))).append('\n')
        }
        "fingerprint" -> {
            val text = root.text("text") ?: ""
            val imageCount = root.integer("imageCount")
            val attachmentMeta = root.text("attachmentMeta")
            out.append("args|").append(esc(text)).append('|').append(imageCount).append('|')
                .append(attachmentMeta?.let { esc(it) } ?: "-").append('\n')
            out.append("result|")
                .append(esc(regenerationInputFingerprint(text, imageCount, attachmentMeta)))
                .append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
