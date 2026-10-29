// Golden trace driver for viewmodel/GenerationPolicies.kt and
// viewmodel/AutomaticMessageApiProjection.kt.
//
// The two policy files are lifted verbatim by `agora_extract widen`, which only widens `internal`
// to the default visibility so the driver can call them; no logic is changed.
//
// Every policy of the two files is exercised: the terminal-disposition rules, the two throttle
// gates, the thought-segment tools, the message templates and the API-only history projection.
// The JVM runs with a fixed time zone (UTC), so the templated timestamps are deterministic.
package generation.policies.golden

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageSource
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.viewmodel.automaticMessageApiText
import com.newoether.agora.viewmodel.applyMessageTemplatesToMessages
import com.newoether.agora.viewmodel.applyUserTemplateToMessages
import com.newoether.agora.viewmodel.generationTerminalDisposition
import com.newoether.agora.viewmodel.projectAutomaticMessagesForApi
import com.newoether.agora.viewmodel.projectGenerationInputMessages
import com.newoether.agora.viewmodel.shouldPostGenerationTerminalNotification
import com.newoether.agora.viewmodel.toolRoundThoughtSegments
import com.newoether.agora.viewmodel.StreamingCheckpointGate
import com.newoether.agora.viewmodel.StreamingUiUpdateGate
import com.newoether.agora.viewmodel.ToolRoundHistoryCompactor
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

private fun buildSegment(spec: JsonObject): MessageSegment = MessageSegment(
    type = (spec["type"] as JsonPrimitive).content,
    content = (spec["content"] as? JsonPrimitive)?.contentOrNull ?: "",
    signature = (spec["signature"] as? JsonPrimitive)?.contentOrNull,
    signatureProvider = (spec["signatureProvider"] as? JsonPrimitive)?.contentOrNull,
    durationMs = (spec["durationMs"] as? JsonPrimitive)?.contentOrNull?.toLong(),
    errorCode = (spec["errorCode"] as? JsonPrimitive)?.contentOrNull,
)

private fun buildMessage(spec: JsonObject): ChatMessage = ChatMessage(
    id = (spec["id"] as JsonPrimitive).content,
    parentId = (spec["parentId"] as? JsonPrimitive)?.contentOrNull,
    text = (spec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
    images = (spec["images"] as? JsonArray)?.map { (it as JsonPrimitive).content } ?: emptyList(),
    status = when ((spec["status"] as? JsonPrimitive)?.contentOrNull) {
        "ERROR" -> MessageStatus.ERROR
        "STOPPED" -> MessageStatus.STOPPED
        "THINKING" -> MessageStatus.THINKING
        "SENDING" -> MessageStatus.SENDING
        "TOOL_CALLING" -> MessageStatus.TOOL_CALLING
        "TRANSCRIBING" -> MessageStatus.TRANSCRIBING
        else -> MessageStatus.SUCCESS
    },
    participant = when ((spec["participant"] as? JsonPrimitive)?.contentOrNull) {
        "MODEL" -> Participant.MODEL
        "ERROR" -> Participant.ERROR
        else -> Participant.USER
    },
    timestamp = (spec["timestamp"] as? JsonPrimitive)?.contentOrNull?.toLong() ?: 0L,
    modelName = (spec["modelName"] as? JsonPrimitive)?.contentOrNull,
    segments = (spec["segments"] as? JsonArray)?.map { buildSegment(it.jsonObject) },
    runId = (spec["runId"] as? JsonPrimitive)?.contentOrNull,
    source = (spec["source"] as? JsonObject)?.let { source ->
        val kind = when ((source["kind"] as? JsonPrimitive)?.contentOrNull) {
            "loop" -> MessageSource.Kind.LOOP
            "ask_user" -> MessageSource.Kind.ASK_USER
            else -> MessageSource.Kind.TASK
        }
        val askUser = (source["askUser"] as? JsonArray)?.map { item ->
            MessageSource.AskUserItem(
                question = (item.jsonObject["question"] as JsonPrimitive).content,
                answer = (item.jsonObject["answer"] as? JsonPrimitive)?.contentOrNull,
            )
        } ?: emptyList()
        MessageSource(kind, askUser)
    },
)

private fun renderMessage(message: ChatMessage): JsonObject = buildJsonObject {
    put("id", message.id)
    put("parentId", message.parentId)
    put("text", message.text)
    put("images", buildJsonArray { message.images.forEach { add(it) } })
    put("status", message.status.name)
    put("participant", message.participant.name)
    put("timestamp", message.timestamp)
    put("modelName", message.modelName)
    put("segments", buildJsonArray {
        message.segments.orEmpty().forEach { segment ->
            add(buildJsonObject {
                put("type", segment.type)
                put("content", segment.content)
                put("signature", segment.signature)
                put("signatureProvider", segment.signatureProvider)
                put("durationMs", segment.durationMs)
                put("errorCode", segment.errorCode)
            })
        }
    })
}

private fun renderMessages(messages: List<ChatMessage>): JsonArray = buildJsonArray {
    messages.forEach { add(renderMessage(it)) }
}

private fun buildSource(spec: JsonObject): MessageSource {
    val kind = when ((spec["kind"] as? JsonPrimitive)?.contentOrNull) {
        "loop" -> MessageSource.Kind.LOOP
        "ask_user" -> MessageSource.Kind.ASK_USER
        else -> MessageSource.Kind.TASK
    }
    val askUser = (spec["askUser"] as? JsonArray)?.map { item ->
        MessageSource.AskUserItem(
            question = (item.jsonObject["question"] as JsonPrimitive).content,
            answer = (item.jsonObject["answer"] as? JsonPrimitive)?.contentOrNull,
        )
    } ?: emptyList()
    return MessageSource(kind, askUser)
}

private fun render(source: MessageSource): JsonObject = buildJsonObject {
    put("kind", source.kind.name)
    put("askUser", buildJsonArray {
        source.askUser.forEach { item ->
            add(buildJsonObject {
                put("question", item.question)
                put("answer", item.answer)
            })
        }
    })
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
    val messages = (root["messages"] as? JsonArray)?.map { buildMessage(it.jsonObject) } ?: emptyList()

    val trace = buildJsonObject {
        // --- terminal disposition ------------------------------------------------
        put("terminalNotifications", buildJsonArray {
            for (status in listOf(
                MessageStatus.SUCCESS, MessageStatus.ERROR, MessageStatus.STOPPED,
                MessageStatus.THINKING,
            )) {
                for (pending in listOf(false, true)) {
                    for (compact in listOf(false, true)) {
                        for (foreground in listOf(false, true)) {
                            for (visible in listOf(null, false, true)) {
                                add(buildJsonObject {
                                    put("status", status.name)
                                    put("pending", pending)
                                    put("compact", compact)
                                    put("foreground", foreground)
                                    put("visible", visible)
                                    put(
                                        "notify",
                                        shouldPostGenerationTerminalNotification(
                                            status, pending, compact, foreground, visible,
                                        ),
                                    )
                                })
                            }
                        }
                    }
                }
            }
        })
        put("terminalDispositions", buildJsonArray {
            for (status in listOf(
                MessageStatus.SUCCESS, MessageStatus.ERROR, MessageStatus.STOPPED,
                MessageStatus.THINKING,
            )) {
                for (pending in listOf(false, true)) {
                    for (visible in listOf(null, false, true)) {
                        val disposition = generationTerminalDisposition(status, pending, visible)
                        add(buildJsonObject {
                            put("status", status.name)
                            put("pending", pending)
                            put("visible", visible)
                            put("runStatus", disposition.runStatus.name)
                            put("endReason", disposition.endReason.name)
                            put("markUnread", disposition.markConversationUnread)
                        })
                    }
                }
            }
        })

        // --- throttle gates ------------------------------------------------------
        val checkpoint = StreamingCheckpointGate(intervalMs = 1_000L)
        put("checkpointGate", buildJsonArray {
            for (now in listOf(0L, 500L, 999L, 1_000L, 1_500L, 2_500L, 100L)) {
                add(buildJsonObject {
                    put("now", now)
                    put("checkpoint", checkpoint.shouldCheckpoint(now))
                })
            }
        })
        val uiGate = StreamingUiUpdateGate(intervalMs = 50L)
        put("uiGate", buildJsonArray {
            for (now in listOf(0L, 10L, 49L, 50L, 51L, 5L)) {
                val due = uiGate.isDue(now)
                if (due) uiGate.recordPublished(now)
                add(buildJsonObject {
                    put("now", now)
                    put("due", due)
                })
            }
        })

        // --- thought segments ----------------------------------------------------
        val segments = (root["segments"] as? JsonArray)?.map { buildSegment(it.jsonObject) } ?: emptyList()
        put("thoughtSegments", buildJsonArray {
            for (from in listOf(-1, 0, 1, 2, 99)) {
                add(buildJsonArray {
                    toolRoundThoughtSegments(segments, from).forEach { segment ->
                        add(buildJsonObject {
                            put("type", segment.type)
                            put("content", segment.content)
                        })
                    }
                })
            }
        })
        val compactor = ToolRoundHistoryCompactor()
        put("compacted", buildJsonArray {
            for (round in (root["compactorRounds"] as? JsonArray) ?: emptyList()) {
                val spec = round.jsonObject
                val runId = (spec["runId"] as JsonPrimitive).content
                val roundSegments = (spec["segments"] as? JsonArray)?.map {
                    buildSegment(it.jsonObject)
                } ?: emptyList()
                val compacted = compactor.compact(runId, roundSegments)
                add(buildJsonArray {
                    compacted.forEach { segment ->
                        add(buildJsonObject {
                            put("type", segment.type)
                            put("content", segment.content)
                        })
                    }
                })
            }
        })

        // --- templates -----------------------------------------------------------
        val templates = (root["templates"] as? JsonObject) ?: JsonObject(emptyMap())
        val userPrepend = (templates["userPrepend"] as? JsonPrimitive)?.contentOrNull
        val userPostpend = (templates["userPostpend"] as? JsonPrimitive)?.contentOrNull
        val assistantPrepend = (templates["assistantPrepend"] as? JsonPrimitive)?.contentOrNull
        val assistantPostpend = (templates["assistantPostpend"] as? JsonPrimitive)?.contentOrNull
        put("templated", renderMessages(
            applyMessageTemplatesToMessages(
                messages, userPrepend, userPostpend, assistantPrepend, assistantPostpend,
            ),
        ))
        put("userTemplated", renderMessages(
            applyUserTemplateToMessages(messages, userPrepend, userPostpend),
        ))

        // --- API projection ------------------------------------------------------
        put("automatic", renderMessages(projectAutomaticMessagesForApi(messages)))
        val sourceSpec = root["automaticSource"] as? JsonObject
        if (sourceSpec != null) {
            put("automaticText", automaticMessageApiText(
                buildSource(sourceSpec),
                (sourceSpec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
            ))
        }
        val projection = root["projection"] as? JsonObject
        if (projection != null) {
            val includeImages = (projection["includeImages"] as? JsonPrimitive)?.content == "true"
            val initialPrompt = (projection["initialUserPrompt"] as? JsonPrimitive)?.contentOrNull
            put("projected", renderMessages(
                projectGenerationInputMessages(
                    messages = messages,
                    includeImages = includeImages,
                    userPrepend = userPrepend,
                    userPostpend = userPostpend,
                    assistantPrepend = assistantPrepend,
                    assistantPostpend = assistantPostpend,
                    initialUserPrompt = initialPrompt,
                ),
            ))
        }
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
