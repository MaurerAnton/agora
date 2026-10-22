// Golden trace driver for viewmodel/GenerationStreamingSegments.kt (lifted verbatim with a widened
// visibility so the driver can call it).
//
// The scenario drives the thought timing, the segment merge, the live-segment builder, the citation
// rebase, the bounded output transform and the final snapshot.
package generation.streaming.segments.golden

import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.CitationAnchor
import com.newoether.agora.model.CitationRecord
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.StreamingTextDelta
import com.newoether.agora.viewmodel.GenerationFinalSnapshot
import com.newoether.agora.viewmodel.GenerationThoughtTiming
import com.newoether.agora.viewmodel.appendMergedSegment
import com.newoether.agora.viewmodel.buildLiveSegments
import com.newoether.agora.viewmodel.rebaseCitationForFinalAnswer
import com.newoether.agora.viewmodel.statusAfterThoughtPhaseFinished
import com.newoether.agora.viewmodel.terminalGenerationErrorMessage
import com.newoether.agora.viewmodel.withBoundedOutputTextTransform
import com.newoether.agora.viewmodel.toMessage
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
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
    streamingTextDeltas = (spec["streamingTextDeltas"] as? JsonArray)?.map { delta ->
        StreamingTextDelta(
            sequence = (delta.jsonObject["sequence"] as JsonPrimitive).content.toLong(),
            codePointCount = (delta.jsonObject["codePointCount"] as JsonPrimitive).content.toInt(),
        )
    } ?: emptyList(),
)

private fun buildMessage(spec: JsonObject): ChatMessage = ChatMessage(
    id = (spec["id"] as JsonPrimitive).content,
    parentId = (spec["parentId"] as? JsonPrimitive)?.contentOrNull,
    text = (spec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
    status = when ((spec["status"] as? JsonPrimitive)?.contentOrNull) {
        "ERROR" -> MessageStatus.ERROR
        "STOPPED" -> MessageStatus.STOPPED
        "THINKING" -> MessageStatus.THINKING
        else -> MessageStatus.SUCCESS
    },
    participant = when ((spec["participant"] as? JsonPrimitive)?.contentOrNull) {
        "MODEL" -> Participant.MODEL
        "ERROR" -> Participant.ERROR
        else -> Participant.USER
    },
    timestamp = (spec["timestamp"] as? JsonPrimitive)?.contentOrNull?.toLong() ?: 0L,
    segments = (spec["segments"] as? JsonArray)?.map { buildSegment(it.jsonObject) },
)

private fun buildCitation(spec: JsonObject): CitationRecord = CitationRecord(
    sourceId = (spec["sourceId"] as JsonPrimitive).content,
    provider = (spec["provider"] as? JsonPrimitive)?.contentOrNull ?: "openai",
    kind = (spec["kind"] as? JsonPrimitive)?.contentOrNull ?: "url",
    title = (spec["title"] as? JsonPrimitive)?.contentOrNull ?: "",
    url = (spec["url"] as? JsonPrimitive)?.contentOrNull,
    anchors = (spec["anchors"] as? JsonArray)?.map { anchor ->
        CitationAnchor(
            startIndex = (anchor.jsonObject["startIndex"] as JsonPrimitive).content.toInt(),
            endIndex = (anchor.jsonObject["endIndex"] as JsonPrimitive).content.toInt(),
            citedText = (anchor.jsonObject["citedText"] as? JsonPrimitive)?.contentOrNull ?: "",
        )
    } ?: emptyList(),
)

private fun renderSegment(segment: MessageSegment): JsonObject = buildJsonObject {
    put("type", segment.type)
    put("content", segment.content)
    put("signature", segment.signature)
    put("signatureProvider", segment.signatureProvider)
    put("durationMs", segment.durationMs)
    put("errorCode", segment.errorCode)
    put("deltas", buildJsonArray {
        segment.streamingTextDeltas.forEach { delta ->
            add(buildJsonObject {
                put("sequence", delta.sequence)
                put("codePointCount", delta.codePointCount)
            })
        }
    })
}

private fun renderSegments(segments: List<MessageSegment>?): JsonElement = buildJsonArray {
    segments.orEmpty().forEach { add(renderSegment(it)) }
}

private fun renderCitation(citation: CitationRecord): JsonObject = buildJsonObject {
    put("sourceId", citation.sourceId)
    put("provider", citation.provider)
    put("kind", citation.kind)
    put("title", citation.title)
    put("url", citation.url)
    put("anchors", buildJsonArray {
        citation.anchors.forEach { anchor ->
            add(buildJsonObject {
                put("startIndex", anchor.startIndex)
                put("endIndex", anchor.endIndex)
                put("citedText", anchor.citedText)
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
    val trace = buildJsonObject {
        // --- thought timing ---------------------------------------------------------
        val clock = (root["clock"] as? JsonArray)?.map { (it as JsonPrimitive).content.toLong() }
            ?: listOf(0L)
        var clockIndex = 0
        val timing = GenerationThoughtTiming {
            val value = clock.getOrElse(clockIndex) { clock.lastOrNull() ?: 0L }
            clockIndex++
            value
        }
        put("timing", buildJsonArray {
            for (step in (root["timingSteps"] as? JsonArray) ?: emptyList()) {
                val action = (step.jsonObject["action"] as JsonPrimitive).content
                when (action) {
                    "ensureStarted" -> timing.ensureStarted()
                    "live" -> Unit
                    "finish" -> timing.finishCurrent()
                    "reset" -> timing.resetCurrentDuration()
                    "adopt" -> timing.adoptTotalDuration(
                        (step.jsonObject["duration"] as? JsonPrimitive)?.contentOrNull?.toLong(),
                    )
                }
                add(buildJsonObject {
                    put("action", action)
                    put("current", timing.currentDurationMs)
                    put("total", timing.totalDurationMs)
                    put("live", timing.liveDurationMs())
                })
            }
        })

        // --- status after the thought phase ------------------------------------------
        put("statusAfterThought", buildJsonArray {
            for (status in listOf(
                MessageStatus.THINKING, MessageStatus.SENDING, MessageStatus.SUCCESS,
                MessageStatus.ERROR, MessageStatus.STOPPED,
            )) {
                add(buildJsonObject {
                    put("status", status.name)
                    put("after", statusAfterThoughtPhaseFinished(status).name)
                })
            }
        })

        // --- segment merge -----------------------------------------------------------
        val merged = mutableListOf<MessageSegment>()
        put("merged", buildJsonArray {
            for (spec in (root["merge"] as? JsonArray) ?: emptyList()) {
                appendMergedSegment(merged, buildSegment(spec.jsonObject))
                add(renderSegment(merged.last()))
            }
        })

        // --- live segments -----------------------------------------------------------
        val live = root["live"] as? JsonObject
        if (live != null) {
            val flushed = (live["flushed"] as? JsonArray)?.map { buildSegment(it.jsonObject) }
                ?: emptyList()
            val segments = buildLiveSegments(
                flushed = flushed,
                answer = (live["answer"] as? JsonPrimitive)?.contentOrNull ?: "",
                thought = (live["thought"] as? JsonPrimitive)?.contentOrNull ?: "",
                signature = (live["signature"] as? JsonPrimitive)?.contentOrNull,
                signatureProvider = (live["signatureProvider"] as? JsonPrimitive)?.contentOrNull,
                thoughtDurationMs = (live["thoughtDurationMs"] as? JsonPrimitive)
                    ?.contentOrNull?.toLong(),
                errorMessage = (live["errorMessage"] as? JsonPrimitive)?.contentOrNull,
                errorCode = (live["errorCode"] as? JsonPrimitive)?.contentOrNull,
                answerDeltas = (live["answerDeltas"] as? JsonArray)?.map { delta ->
                    StreamingTextDelta(
                        sequence = (delta.jsonObject["sequence"] as JsonPrimitive).content.toLong(),
                        codePointCount =
                            (delta.jsonObject["codePointCount"] as JsonPrimitive).content.toInt(),
                    )
                } ?: emptyList(),
            )
            put("liveSegments", renderSegments(segments))
        }

        // --- citation rebase ---------------------------------------------------------
        if (root["citation"] != null) {
            val citation = buildCitation(root["citation"]!!.jsonObject)
            val providerStart =
                (root["providerAnswerStart"] as? JsonPrimitive)?.contentOrNull?.toInt() ?: 0
            val finalAnswer = (root["finalAnswer"] as? JsonPrimitive)?.contentOrNull ?: ""
            put("rebased", renderCitation(
                rebaseCitationForFinalAnswer(citation, providerStart, finalAnswer),
            ))
        }

        // --- bounded output transform ------------------------------------------------
        val message = root["message"]?.let { buildMessage(it.jsonObject) }
        if (message != null) {
            val transformed = message.withBoundedOutputTextTransform { text, status ->
                when ((root["transform"] as? JsonObject)?.let { spec ->
                    (spec["prefix"] as? JsonPrimitive)?.contentOrNull.orEmpty() +
                        (spec["suffix"] as? JsonPrimitive)?.contentOrNull.orEmpty()
                }) {
                    null -> text
                    else -> (root["transform"] as JsonObject)["replacement"]?.let {
                        (it as JsonPrimitive).content
                    } ?: text
                }
            }
            put("bounded", buildJsonObject {
                put("text", transformed.text)
                put("segments", renderSegments(transformed.segments))
            })
        }

        // --- terminal error ----------------------------------------------------------
        if (root["terminalError"] != null) {
            val spec = root["terminalError"]!!.jsonObject
            val status = when ((spec["status"] as? JsonPrimitive)?.contentOrNull) {
                "ERROR" -> MessageStatus.ERROR
                "STOPPED" -> MessageStatus.STOPPED
                else -> MessageStatus.SUCCESS
            }
            val terminalError = terminalGenerationErrorMessage(
                status,
                (spec["currentError"] as? JsonPrimitive)?.contentOrNull,
                (spec["fallbackError"] as? JsonPrimitive)?.contentOrNull ?: "",
            )
            if (terminalError == null) put("terminalError", JsonNull) else put("terminalError", terminalError)
        }

        // --- final snapshot ----------------------------------------------------------
        val snapshotSpec = root["snapshot"] as? JsonObject
        if (snapshotSpec != null) {
            val snapshot = GenerationFinalSnapshot(
                messageId = (snapshotSpec["messageId"] as JsonPrimitive).content,
                parentId = (snapshotSpec["parentId"] as? JsonPrimitive)?.contentOrNull,
                text = (snapshotSpec["text"] as? JsonPrimitive)?.contentOrNull ?: "",
                images = (snapshotSpec["images"] as? JsonArray)?.map {
                    (it as JsonPrimitive).content
                } ?: emptyList(),
                thoughts = (snapshotSpec["thoughts"] as? JsonPrimitive)?.contentOrNull ?: "",
                thoughtTitle = (snapshotSpec["thoughtTitle"] as? JsonPrimitive)?.contentOrNull,
                tokenCount = (snapshotSpec["tokenCount"] as? JsonPrimitive)?.contentOrNull?.toInt() ?: 0,
                tokenUsage = null,
                status = MessageStatus.SUCCESS,
                timestamp = (snapshotSpec["timestamp"] as? JsonPrimitive)?.contentOrNull?.toLong() ?: 0,
                thoughtTimeMs = (snapshotSpec["thoughtTimeMs"] as? JsonPrimitive)
                    ?.contentOrNull?.toLong(),
                modelName = (snapshotSpec["modelName"] as? JsonPrimitive)?.contentOrNull ?: "",
                flushedSegments = (snapshotSpec["flushedSegments"] as? JsonArray)?.map {
                    buildSegment(it.jsonObject)
                } ?: emptyList(),
                answerBuffer = (snapshotSpec["answerBuffer"] as? JsonPrimitive)?.contentOrNull ?: "",
                thoughtBuffer = (snapshotSpec["thoughtBuffer"] as? JsonPrimitive)?.contentOrNull ?: "",
                thoughtSignature = (snapshotSpec["thoughtSignature"] as? JsonPrimitive)
                    ?.contentOrNull,
                thoughtSignatureProvider = (snapshotSpec["thoughtSignatureProvider"] as? JsonPrimitive)
                    ?.contentOrNull,
                thoughtDurationMs = (snapshotSpec["thoughtDurationMs"] as? JsonPrimitive)
                    ?.contentOrNull?.toLong(),
                errorMessage = (snapshotSpec["errorMessage"] as? JsonPrimitive)?.contentOrNull,
                errorCode = (snapshotSpec["errorCode"] as? JsonPrimitive)?.contentOrNull,
                runId = (snapshotSpec["runId"] as JsonPrimitive).content,
                runSequence = (snapshotSpec["runSequence"] as JsonPrimitive).content.toLong(),
                answerDeltas = (snapshotSpec["answerDeltas"] as? JsonArray)?.map { delta ->
                    StreamingTextDelta(
                        sequence = (delta.jsonObject["sequence"] as JsonPrimitive).content.toLong(),
                        codePointCount =
                            (delta.jsonObject["codePointCount"] as JsonPrimitive).content.toInt(),
                    )
                } ?: emptyList(),
            )
            val message = snapshot.toMessage()
            put("snapshot", buildJsonObject {
                put("id", message.id)
                put("parentId", message.parentId)
                put("text", message.text)
                put("images", buildJsonArray { message.images.forEach { image -> add(image) } })
                put("thoughts", message.thoughts)
                put("thoughtTitle", message.thoughtTitle)
                put("tokenCount", message.tokenCount)
                put("status", message.status.name)
                put("participant", message.participant.name)
                put("timestamp", message.timestamp)
                put("thoughtTimeMs", message.thoughtTimeMs)
                put("modelName", message.modelName)
                put("runId", message.runId)
                put("runSequence", message.runSequence)
                put("segments", renderSegments(message.segments))
            })
        }
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
