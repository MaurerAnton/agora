// Golden trace driver for model/RunRecoveryPolicy.kt (compiled verbatim from the app tree).
//
// The policy is pure: status enums in, status out; a list of MessageSegment data classes in, a
// list out; or raw JSON in, raw JSON (or null) out. The trace prints the complete Kotlin
// data-class toString() of both segment lists, so every untouched field is observed, including
// the two @Transient ones that kotlinx would omit from a serialized projection.
//
// Operations:
//   recoverMessageStatus   one (participant, status) pair per statuses[] entry
//   stopIncompleteTools    segments[] -> MessageSegment list before and after
//   stopIncompleteToolsJson raw -> nullable string (- means null)
package run.recovery.golden

import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunRecoveryPolicy
import com.newoether.agora.model.StreamingTextDelta
import com.newoether.agora.model.ToolImageAttachment
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.longOrNull
import java.io.File

private val json = Json

/** The trace escaping shared by every golden stand: `|` and `;` are separators. */
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

private fun opt(value: String?): String = value?.let { esc(it) } ?: "-"

private fun JsonObject.str(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.requiredStr(name: String): String =
    str(name) ?: error("scenario field `$name` is required")

private fun JsonObject.int(name: String): Int? =
    (this[name] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.intOrNull

private fun JsonObject.long(name: String): Long? =
    (this[name] as? JsonPrimitive)?.takeIf { it !is JsonNull }?.longOrNull

private fun JsonObject.objectArray(name: String): List<JsonObject> =
    (this[name] as? JsonArray)?.map { it.jsonObject } ?: emptyList()

private fun JsonObject.image(): ToolImageAttachment = ToolImageAttachment(
    path = requiredStr("path"),
    mimeType = requiredStr("mimeType"),
    sizeBytes = long("sizeBytes") ?: 0L,
    width = int("width"),
    height = int("height"),
    sha256 = requiredStr("sha256"),
)

private fun JsonObject.delta(): StreamingTextDelta = StreamingTextDelta(
    sequence = long("sequence") ?: 0L,
    codePointCount = int("codePointCount") ?: 0,
)

/**
 * Builds the real MessageSegment through its public constructor. ChatMessage.kt cannot be
 * compiled here, so the stand supplies the fixture decoder, not the policy.
 */
private fun JsonObject.segment(): MessageSegment = MessageSegment(
    type = requiredStr("type"),
    content = str("content") ?: "",
    toolName = str("toolName"),
    toolArgs = str("toolArgs"),
    toolResult = str("toolResult"),
    toolCallId = str("toolCallId"),
    signature = str("signature"),
    signatureProvider = str("signatureProvider"),
    durationMs = long("durationMs"),
    toolState = str("toolState"),
    toolProgress = str("toolProgress"),
    toolTarget = str("toolTarget"),
    toolDisplayName = str("toolDisplayName"),
    toolResultText = str("toolResultText"),
    toolStructuredResult = str("toolStructuredResult"),
    toolImages = (this["toolImages"] as? JsonArray)?.map { it.jsonObject.image() } ?: emptyList(),
    toolImageRequestKey = str("toolImageRequestKey"),
    toolTranscription = str("toolTranscription"),
    responseOutputItems = objectArray("responseOutputItems"),
    responseOutputItemProvider = str("responseOutputItemProvider"),
    errorCode = str("errorCode"),
    streamingTextDeltas =
        (this["streamingTextDeltas"] as? JsonArray)?.map { it.jsonObject.delta() } ?: emptyList(),
)

private fun JsonObject.segments(): List<MessageSegment> =
    (this["segments"] as? JsonArray)?.map { it.jsonObject.segment() } ?: emptyList()

fun main(args: Array<String>) {
    val root = json.parseToJsonElement(File(args[0]).readText()).jsonObject
    val name = root.str("name") ?: File(args[0]).nameWithoutExtension
    val fn = root.requiredStr("fn")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("fn|").append(esc(fn)).append('\n')

    when (fn) {
        "recoverMessageStatus" -> {
            val participant = Participant.valueOf(root.requiredStr("participant"))
            val statuses = (root["statuses"] as? JsonArray)
                ?.map { (it as JsonPrimitive).content }
                ?: error("scenario field `statuses` is required")
            for (statusName in statuses) {
                val status = MessageStatus.valueOf(statusName)
                out.append("args|").append(participant.name).append('|').append(status.name)
                    .append('\n')
                out.append("result|")
                    .append(RunRecoveryPolicy.recoverMessageStatus(participant, status).name)
                    .append('\n')
            }
        }

        "stopIncompleteTools" -> {
            val segments = root.segments()
            out.append("args|segments|").append(esc(segments.toString())).append('\n')
            out.append("result|segments|")
                .append(esc(RunRecoveryPolicy.stopIncompleteTools(segments).toString()))
                .append('\n')
        }

        "stopIncompleteToolsJson" -> {
            val raw = root.requiredStr("raw")
            out.append("args|raw|").append(esc(raw)).append('\n')
            out.append("result|string|")
                .append(opt(RunRecoveryPolicy.stopIncompleteToolsJson(raw)))
                .append('\n')
        }

        else -> error("unknown function `$fn`")
    }

    print(out)
}
