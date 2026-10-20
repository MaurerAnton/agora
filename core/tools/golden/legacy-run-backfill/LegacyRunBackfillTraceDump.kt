// Golden trace driver for data/local/migration/LegacyRunBackfillPlanner.kt (compiled verbatim).
//
// `plan` prints every PlannedLegacyRun and PlannedMessageAssignment via the data-class toString(),
// which pins the deterministic UUID v3 Run ids, the topology order, the status/end-reason
// derivation and the legacy-ambiguity flags. `selected` additionally runs selectedRunBranches and
// prints the nullable-key selection map. `require` failures print their IllegalArgumentException
// message.
package legacy.run.backfill.golden

import com.newoether.agora.data.local.migration.LegacyMessageRecord
import com.newoether.agora.data.local.migration.LegacyRunBackfillPlan
import com.newoether.agora.data.local.migration.LegacyRunBackfillPlanner
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
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

private fun JsonObject.objects(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it as JsonObject } ?: emptyList()

private fun JsonObject.obj(name: String): JsonObject =
    this[name] as? JsonObject ?: JsonObject(emptyMap())

private fun JsonObject.selections(): LinkedHashMap<String?, String> {
    val out = LinkedHashMap<String?, String>()
    for ((key, value) in this) {
        val text = (value as? JsonPrimitive)?.takeIf { it.isString }?.content ?: continue
        out[if (key == "null") null else key] = text
    }
    return out
}

private fun parseMessages(root: JsonObject): List<LegacyMessageRecord> =
    root.objects("messages").map { item ->
        val id = item.requiredText("id")
        LegacyMessageRecord(
            id = id,
            parentId = item.text("parentId"),
            participant = Participant.valueOf(item.requiredText("participant")),
            status = MessageStatus.valueOf(item.requiredText("status")),
            timestamp = item.long("timestamp"),
            isSynthetic = (item["isSynthetic"] as? JsonPrimitive)?.booleanOrNull
                ?: (id.startsWith("tool_") || id.startsWith("result_")),
        )
    }

private fun renderPlan(plan: LegacyRunBackfillPlan): String =
    "runs=" + plan.runs.joinToString(";") { it.toString() } +
        "|assignments=" + plan.assignments.joinToString(";") { it.toString() }

private fun renderSelections(selections: Map<String?, String>): String =
    selections.entries.joinToString(";") { "${it.key ?: "<null>"}=${it.value}" }

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")
    val conversationId = root.text("conversationId") ?: ""
    val messages = parseMessages(root)

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')
    out.append("args|").append(esc(conversationId)).append('|').append(messages.size).append('\n')

    when (op) {
        "plan" -> try {
            val plan = LegacyRunBackfillPlanner.plan(conversationId, messages)
            out.append("result|").append(esc(renderPlan(plan))).append('\n')
        } catch (error: IllegalArgumentException) {
            out.append("result|ERROR|").append(esc(error.message ?: "")).append('\n')
        }
        "selected" -> try {
            val plan = LegacyRunBackfillPlanner.plan(conversationId, messages)
            val selections = LegacyRunBackfillPlanner.selectedRunBranches(
                messages = messages,
                plan = plan,
                selectedMessageBranches = root.obj("selectedMessageBranches").selections(),
            )
            out.append("result|").append(esc(renderSelections(selections))).append('\n')
        } catch (error: IllegalArgumentException) {
            out.append("result|ERROR|").append(esc(error.message ?: "")).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
