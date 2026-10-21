// Golden trace driver for model/MessageGenerationBoundary.kt and model/RunUiProjection.kt (both
// compiled verbatim from the app tree).
//
// A scenario supplies the visible messages and the full message list; the trace renders the
// resolved boundaries and the per-message presentation map (the UI affordances of every message).
package run.ui.projection.golden

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageGenerationBoundaryResolver
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.RunUiProjection
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import java.io.File

private val json = Json

private fun buildMessage(spec: JsonObject): ChatMessage {
    val id = (spec["id"] as JsonPrimitive).content
    val parentId = (spec["parentId"] as? JsonPrimitive)?.contentOrNull
    val participant = when ((spec["participant"] as? JsonPrimitive)?.contentOrNull) {
        "MODEL" -> Participant.MODEL
        "ERROR" -> Participant.ERROR
        else -> Participant.USER
    }
    val status = when ((spec["status"] as? JsonPrimitive)?.contentOrNull) {
        "ERROR" -> MessageStatus.ERROR
        "STOPPED" -> MessageStatus.STOPPED
        "THINKING" -> MessageStatus.THINKING
        "SENDING" -> MessageStatus.SENDING
        "TOOL_CALLING" -> MessageStatus.TOOL_CALLING
        "TRANSCRIBING" -> MessageStatus.TRANSCRIBING
        else -> MessageStatus.SUCCESS
    }
    return ChatMessage(
        id = id,
        parentId = parentId,
        text = (spec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
        status = status,
        participant = participant,
        timestamp = (spec["timestamp"] as? JsonPrimitive)?.contentOrNull?.toLong() ?: 0L,
        runId = (spec["runId"] as? JsonPrimitive)?.contentOrNull,
    )
}

private fun buildMessages(root: JsonObject, key: String): List<ChatMessage> {
    val list = root[key] as? JsonArray ?: return emptyList()
    return list.map { buildMessage(it.jsonObject) }
}

private fun renderBoundary(boundary: com.newoether.agora.model.MessageGenerationBoundary): JsonObject =
    buildJsonObject {
        put("messages", buildJsonArray { boundary.messages.forEach { add(it.id) } })
        put("input", boundary.input?.id)
        put("firstAssistant", boundary.firstAssistant?.id)
        put("lastAssistant", boundary.lastAssistant?.id)
    }

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String {
    val root = Json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val visible = buildMessages(root, "visibleMessages")
    val all = buildMessages(root, "allMessages")

    val boundaries = MessageGenerationBoundaryResolver.resolve(visible)
    val projection = RunUiProjection.project(visible, all)

    val trace = buildJsonObject {
        put("boundaries", buildJsonArray { boundaries.forEach { add(renderBoundary(it)) } })
        put("presentation", buildJsonObject {
            projection.forEach { (messageId, presentation) ->
                put(messageId, buildJsonObject {
                    put("showActions", presentation.showActions)
                    put("copyText", presentation.copyText)
                    put("deleteTargetMessageId", presentation.deleteTargetMessageId)
                    put("showBranchSelector", presentation.showBranchSelector)
                    put("branchIndex", presentation.branchIndex)
                    put("totalBranches", presentation.totalBranches)
                    put("branchAnchorParentId", presentation.branchAnchorParentId)
                    put("branchAnchorMessageId", presentation.branchAnchorMessageId)
                })
            }
        })
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
