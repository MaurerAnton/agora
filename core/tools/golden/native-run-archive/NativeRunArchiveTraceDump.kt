// Golden trace driver for data/NativeRunArchivePolicy.kt, compiled VERBATIM from the app tree.
//
// The policy is declared over RunEntity, so data/local/RunEntity.kt is also compiled verbatim;
// only its Room annotations and the ChatEntity table it references are stand-local platform
// stubs (see platform/). model/RuntimeIdentity.kt is compiled verbatim for RunStatus and
// RunEndReason. The C++ core must reproduce these traces byte for byte
// (core/tests/native_run_archive_parity_test.cpp).
package native.run.archive.golden

import com.newoether.agora.data.ArchivedMessageRunOwnership
import com.newoether.agora.data.ArchivedRunSnapshot
import com.newoether.agora.data.NativeRunArchivePolicy
import com.newoether.agora.data.local.RunEntity
import com.newoether.agora.model.RunEndReason
import com.newoether.agora.model.RunStatus
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
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
            '=' -> out.append("\\e")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

/** `n` for a null value, `s:` + escaped text for a real one: the two can never collide. */
private fun text(value: String?): String = if (value == null) "n" else "s:" + esc(value)

private fun long(value: Long?): String = if (value == null) "n" else "l:$value"

private fun int(value: Int?): String = if (value == null) "n" else "i:$value"

private fun bool(value: Boolean): String = if (value) "true" else "false"

private fun JsonObject.primitive(name: String): JsonPrimitive? = this[name] as? JsonPrimitive

private fun JsonObject.textOrNull(name: String): String? =
    primitive(name)?.takeIf { it.isString }?.content

private fun JsonObject.text(name: String, default: String = ""): String =
    textOrNull(name) ?: default

private fun JsonObject.longOrNull(name: String): Long? = primitive(name)?.longOrNull

private fun JsonObject.long(name: String, default: Long): Long = longOrNull(name) ?: default

private fun JsonObject.intOrNull(name: String): Int? = primitive(name)?.intOrNull

private fun JsonObject.int(name: String, default: Int): Int = intOrNull(name) ?: default

private fun JsonObject.flag(name: String, default: Boolean): Boolean =
    primitive(name)?.booleanOrNull ?: default

private fun JsonObject.required(name: String): String =
    textOrNull(name) ?: error("`$name` is required")

private fun encodeRun(run: RunEntity): String = buildString {
    append("id=").append(text(run.id))
    append(";conv=").append(text(run.conversationId))
    append(";parent=").append(text(run.parentRunId))
    append(";status=").append(text(run.status.name))
    append(";slot=").append(int(run.activeSlot))
    append(";started=").append(long(run.startedAt))
    append(";checkpoint=").append(long(run.lastCheckpointAt))
    append(";stop=").append(long(run.stopRequestedAt))
    append(";ended=").append(long(run.endedAt))
    append(";reason=").append(text(run.endReason?.name))
    append(";pass=").append(int(run.currentPass))
    append(";ambiguous=").append(bool(run.legacyAmbiguous))
}

private fun encodeSnapshot(snapshot: ArchivedRunSnapshot): String = buildString {
    append("id=").append(text(snapshot.id))
    append(";conv=").append(text(snapshot.conversationId))
    append(";parent=").append(text(snapshot.parentRunId))
    append(";status=").append(text(snapshot.status))
    append(";started=").append(long(snapshot.startedAt))
    append(";checkpoint=").append(long(snapshot.lastCheckpointAt))
    append(";stop=").append(long(snapshot.stopRequestedAt))
    append(";ended=").append(long(snapshot.endedAt))
    append(";reason=").append(text(snapshot.endReason))
    append(";pass=").append(int(snapshot.currentPass))
    append(";ambiguous=").append(bool(snapshot.legacyAmbiguous))
}

private fun encodeOwnership(item: ArchivedMessageRunOwnership): String = buildString {
    append("message=").append(text(item.messageId))
    append(";conv=").append(text(item.conversationId))
    append(";run=").append(text(item.runId))
    append(";sequence=").append(long(item.runSequence))
    append(";consumed=").append(int(item.consumedAtPass))
}

private fun parseSnapshot(root: JsonObject): ArchivedRunSnapshot = ArchivedRunSnapshot(
    id = root.required("id"),
    conversationId = root.text("conversationId", "conv-a"),
    parentRunId = root.textOrNull("parentRunId"),
    status = root.text("status"),
    startedAt = root.long("startedAt", 0L),
    lastCheckpointAt = root.long("lastCheckpointAt", 0L),
    stopRequestedAt = root.longOrNull("stopRequestedAt"),
    endedAt = root.longOrNull("endedAt"),
    endReason = root.textOrNull("endReason"),
    currentPass = root.int("currentPass", 0),
    legacyAmbiguous = root.flag("legacyAmbiguous", false),
)

private fun parseRun(element: JsonObject): RunEntity {
    val status = RunStatus.valueOf(element.text("status", "COMPLETED"))
    val startedAt = element.long("startedAt", 1L)
    val terminal = status.isTerminal
    return RunEntity(
        id = element.required("id"),
        conversationId = element.text("conversationId", "conv-a"),
        parentRunId = element.textOrNull("parentRunId"),
        status = status,
        activeSlot = if (terminal) null else 1,
        startedAt = startedAt,
        lastCheckpointAt = element.long("lastCheckpointAt", startedAt),
        stopRequestedAt = if (status == RunStatus.STOPPING) {
            element.longOrNull("stopRequestedAt") ?: startedAt
        } else {
            element.longOrNull("stopRequestedAt")
        },
        endedAt = if (terminal) element.longOrNull("endedAt") ?: (startedAt + 1) else null,
        endReason = if (terminal) {
            RunEndReason.valueOf(element.text("endReason", "MODEL_COMPLETED"))
        } else {
            null
        },
        currentPass = element.int("currentPass", 0),
        legacyAmbiguous = element.flag("legacyAmbiguous", false),
    )
}

private fun parseRuns(root: JsonObject): List<RunEntity> {
    val elements = root["runs"] as? JsonArray ?: JsonArray(emptyList())
    return elements.map { parseRun(it.jsonObject) }
}

private fun parseOwnership(root: JsonObject): List<ArchivedMessageRunOwnership> {
    val elements = root["ownership"] as? JsonArray ?: JsonArray(emptyList())
    return elements.map { element ->
        val fields = element.jsonObject
        ArchivedMessageRunOwnership(
            messageId = fields.required("messageId"),
            conversationId = fields.text("conversationId", "conv-a"),
            runId = fields.textOrNull("runId"),
            runSequence = fields.longOrNull("runSequence"),
            consumedAtPass = fields.intOrNull("consumedAtPass"),
        )
    }
}

fun main(args: Array<String>) {
    val path = File(args[0])
    val root = Json.parseToJsonElement(path.readText()).jsonObject
    val name = root.textOrNull("name") ?: path.name.removeSuffix(".json")
    val op = root.required("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "terminalize" -> {
            val snapshot = parseSnapshot(root)
            val result = NativeRunArchivePolicy.terminalize(snapshot)
            out.append("snapshot|").append(encodeSnapshot(snapshot)).append('\n')
            out.append("out|").append(encodeRun(result)).append('\n')
        }
        "hasCompleteOwnership" -> {
            val runs = parseRuns(root)
            val ownership = parseOwnership(root)
            val unique = root.flag("sourceRunIdsWereUnique", true)
            val result = NativeRunArchivePolicy.hasCompleteOwnership(runs, ownership, unique)
            out.append("runs|")
                .append(runs.joinToString("|") { encodeRun(it) })
                .append('\n')
            out.append("ownership|")
                .append(ownership.joinToString("|") { encodeOwnership(it) })
                .append('\n')
            out.append("sourceRunIdsWereUnique|").append(bool(unique)).append('\n')
            out.append("out|").append(bool(result)).append('\n')
        }
        "orderByParent" -> {
            val runs = parseRuns(root)
            val result = NativeRunArchivePolicy.orderByParent(runs)
            out.append("input|")
                .append(runs.joinToString("|") { encodeRun(it) })
                .append('\n')
            out.append("out|")
                .append(result.joinToString("|") { encodeRun(it) })
                .append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
