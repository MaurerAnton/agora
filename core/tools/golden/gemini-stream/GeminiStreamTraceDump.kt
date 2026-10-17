// Golden trace driver for the Gemini stream read loop, used to pin the C++ port.
//
// Feeds raw SSE lines through a faithful transcription of GeminiProvider.generateResponse's read
// loop over the REAL Kotlin sources: the wire models (GeminiWireModels.kt), the citation
// projection (GeminiCitationProjection.kt), the stream helpers extracted verbatim from
// GeminiProvider.kt (thought title, finish reason, termination, hosted-tool projections), the
// real StreamEvent/StreamTermination/MalformedToolCall types and the real carriesModelOutput
// rule. The C++ core must reproduce the same trace byte for byte.
//
// Scenario format: {"name", "providerName", "ops": ["L data: {...}", "E"]}. The ops are:
//   L <line>  feed one raw SSE line through the loop (ignored once the loop stopped)
//   E         end of stream (the loop's readLine returned null)
//
// Trace format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package gemini.stream.golden

import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.gemini.ApiStreamResponse
import com.newoether.agora.api.gemini.extractThoughtTitle
import com.newoether.agora.api.gemini.geminiStreamTermination
import com.newoether.agora.api.gemini.normalizeGeminiFinishReason
import com.newoether.agora.api.gemini.toCodeExecutionCompletion
import com.newoether.agora.api.gemini.toCodeExecutionStart
import com.newoether.agora.api.gemini.toGeminiCitations
import com.newoether.agora.api.gemini.toGoogleSearchHostedUpdate
import com.newoether.agora.api.gemini.toTokenUsage
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.api.util.malformedToolCallRequest
import com.newoether.agora.api.util.safeWireToolCallId
import com.newoether.agora.api.util.safeWireToolName
import com.newoether.agora.model.CitationRecord
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.File
import java.util.UUID

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|[0-9a-f]{32}",
)

/** Random UUIDs (stream keys, malformed stand-in ids) are not comparable across implementations;
 *  both the dashed and the compact forms collapse, like the anthropic-stream stand. */
private fun norm(value: String): String = uuidPattern.replace(value) { "<uuid>" }

/** Field-safe escaping for the trace format: no raw separators or control characters survive. */
private fun esc(value: String): String = buildString {
    for (c in value) {
        when (c) {
            '\\' -> append("\\\\")
            '|' -> append("\\p")
            ';' -> append("\\s")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(c)
        }
    }
}

private fun hex(bytes: ByteArray): String = bytes.joinToString("") { "%02x".format(it) }

@Serializable
private data class Scenario(
    val name: String,
    val providerName: String = "Google",
    val ops: List<String> = emptyList(),
)

private fun usageLine(usage: com.newoether.agora.model.TokenUsage): String =
    "usage|total=${usage.totalTokenCount}" +
        "|input=${usage.inputTokenCount ?: "-"}" +
        "|cached=${usage.cachedInputTokenCount ?: "-"}" +
        "|cacheWrite=${usage.cacheWriteInputTokenCount ?: "-"}" +
        "|uncached=${usage.uncachedInputTokenCount ?: "-"}" +
        "|output=${usage.outputTokenCount ?: "-"}" +
        "|reasoning=${usage.reasoningTokenCount ?: "-"}"

private fun anchorField(anchor: com.newoether.agora.model.CitationAnchor): String =
    "{${anchor.startIndex},${anchor.endIndex},${esc(anchor.citedText)}}"

/** Canonical record rendering shared with the citations/anthropic stands. */
private fun recordFields(record: CitationRecord): String = buildString {
    append("v=${record.version}")
    append(";sourceId=${esc(record.sourceId)}")
    append(";provider=${esc(record.provider)}")
    append(";kind=${esc(record.kind)}")
    append(";title=${esc(record.title)}")
    append(";url=${record.url?.let { esc(it) } ?: "null"}")
    append(";fileName=${record.fileName?.let { esc(it) } ?: "null"}")
    append(";location=${record.location?.let { esc(it) } ?: "null"}")
    append(";providerSourceId=${record.providerSourceId?.let { esc(it) } ?: "null"}")
    append(";excerpt=${record.excerpt?.let { esc(it) } ?: "null"}")
    append(";anchors=[")
    record.anchors.forEachIndexed { index, anchor ->
        if (index > 0) append(',')
        append(anchorField(anchor))
    }
    append(']')
}

private fun eventLine(event: StreamEvent): String = when (event) {
    is StreamEvent.TextChunk -> "text|" + esc(event.text)
    is StreamEvent.ThoughtChunk ->
        "thought|" + esc(event.thought) +
            "|title=${event.title?.let { esc(it) } ?: "-"}" +
            "|signature=${event.signature?.let { esc(it) } ?: "-"}"
    is StreamEvent.ToolCallUpdate ->
        "toolCallUpdate|streamKey=${norm(event.streamKey)}" +
            "|id=${event.id?.let { norm(it) } ?: "-"}" +
            "|name=${esc(event.name)}|args=${esc(event.arguments)}" +
            "|signature=${event.signature?.let { esc(it) } ?: "-"}"
    is StreamEvent.ToolCallRequest ->
        "toolCall|id=${norm(event.id)}|name=${esc(event.name)}|args=${esc(event.arguments)}" +
            "|streamKey=${norm(event.streamKey)}" +
            "|signature=${event.signature?.let { esc(it) } ?: "-"}"
    is StreamEvent.HostedToolCallUpdate ->
        "hosted|streamKey=${norm(event.streamKey)}|name=${esc(event.name)}" +
            "|arguments=${esc(event.arguments)}" +
            "|result=${event.result?.let { esc(it) } ?: "-"}" +
            "|error=${if (event.isError) 1 else 0}"
    is StreamEvent.CitationUpdate -> "citation|" + recordFields(event.citation)
    is StreamEvent.UsageUpdate -> usageLine(event.usage)
    else -> error("unexpected event in the loop: ${event::class.simpleName}")
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: GeminiStreamTraceDump <scenario.json> <filesRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", "stream")

    // ---- the provider loop's 200-branch state -------------------------------------------------
    val providerName = scenario.providerName
    var currentThoughtSignature: String? = null
    var inThoughtBlock = false
    var producedContent = false
    var finishReason: String? = null
    var sawDone = false
    var streamError: com.newoether.agora.api.GenerationError? = null
    var toolCallInFlight = false  // never set by the loop; kept to mirror the termination input
    val completedToolCallIds = mutableSetOf<String>()
    val pendingCodeExecutions = java.util.ArrayDeque<Pair<String, String>>()
    val googleSearchStreamKey = "gemini_google_search_${UUID.randomUUID()}"
    var lastGroundingMetadata: String? = null
    var latestGroundingMetadata: JsonObject? = null
    val answerText = StringBuilder()

    fun emitTracked(event: StreamEvent) {
        if (event.carriesModelOutput()) producedContent = true
        out.append(eventLine(event)).append('\n')
    }

    var stopped = false
    for (op in scenario.ops) {
        if (stopped) continue
        if (op == "E") {
            stopped = true  // readLine returned null: the loop exits at its condition
            continue
        }
        if (!op.startsWith("L ")) error("unknown op: $op")
        val lineText = op.substring(2)
        // -- the loop body, in provider order -------------------------------------------------
        if (!lineText.startsWith("data: ")) continue
        val jsonStr = lineText.substring(6).trim()
        if (jsonStr == "[DONE]") {
            sawDone = true
            stopped = true
            continue
        }
        val response: ApiStreamResponse
        try {
            response = traceJson.decodeFromString(ApiStreamResponse.serializer(), jsonStr)
        } catch (e: Exception) {
            streamError = com.newoether.agora.api.GenerationError.SseParse(
                rawLine = jsonStr.take(512),
                cause = e.localizedMessage ?: "Malformed SSE payload",
            )
            stopped = true
            continue
        }
        response.error?.let { error ->
            streamError = com.newoether.agora.api.GenerationError.Api(
                code = error.code?.toString(),
                type = error.status,
                message = error.message?.ifBlank {
                    "$providerName reported an error in the response stream"
                } ?: "$providerName reported an error in the response stream",
            )
        }
        if (streamError == null &&
            com.newoether.agora.api.util.ProviderRetryPolicy
                .isFailedToGenerateOutcome(response.outcome)
        ) {
            streamError = com.newoether.agora.api.GenerationError.Api(
                null, "failed_to_generate", response.outcome.orEmpty(),
            )
        }
        val candidate = response.candidates?.firstOrNull()
        candidate?.finishReason?.takeIf(String::isNotBlank)?.let {
            finishReason = normalizeGeminiFinishReason(it)
        }
        candidate?.groundingMetadata?.let { metadata ->
            latestGroundingMetadata = metadata
            val snapshot = metadata.toString()
            if (snapshot != lastGroundingMetadata) {
                lastGroundingMetadata = snapshot
                emitTracked(metadata.toGoogleSearchHostedUpdate(googleSearchStreamKey))
            }
        }
        inThoughtBlock = false
        candidate?.content?.parts?.forEach { part ->
            var isPartOfThought = false
            val partThoughtSignature = part.thoughtSignature?.takeIf(String::isNotBlank)
            partThoughtSignature?.let { signature ->
                currentThoughtSignature = signature
                isPartOfThought = true
                inThoughtBlock = true
            }
            part.thought?.let { thoughtElement ->
                if (thoughtElement is kotlinx.serialization.json.JsonPrimitive) {
                    if (thoughtElement.isString) {
                        val content = thoughtElement.content
                        content.takeIf(String::isNotEmpty)?.let {
                            emitTracked(
                                StreamEvent.ThoughtChunk(
                                    it,
                                    extractThoughtTitle(it),
                                    currentThoughtSignature,
                                ),
                            )
                            isPartOfThought = true
                            inThoughtBlock = true
                        }
                    } else if (thoughtElement.content == "true") {
                        isPartOfThought = true
                        inThoughtBlock = true
                    }
                }
            }
            part.reasoningContent?.takeIf(String::isNotEmpty)?.let {
                emitTracked(
                    StreamEvent.ThoughtChunk(
                        it, extractThoughtTitle(it), currentThoughtSignature,
                    ),
                )
                isPartOfThought = true
                inThoughtBlock = true
            }
            part.text?.takeIf(String::isNotEmpty)?.let {
                if (isPartOfThought || inThoughtBlock) {
                    emitTracked(
                        StreamEvent.ThoughtChunk(
                            it, extractThoughtTitle(it), currentThoughtSignature,
                        ),
                    )
                    inThoughtBlock = false
                } else {
                    answerText.append(it)
                    emitTracked(StreamEvent.TextChunk(it))
                }
            }
            part.executableCode?.let { code ->
                val active = code.toCodeExecutionStart(
                    streamKey = "gemini_code_execution_${UUID.randomUUID()}",
                )
                pendingCodeExecutions.addLast(active.streamKey to active.arguments)
                emitTracked(active)
            }
            part.codeExecutionResult?.let { result ->
                val pending = pendingCodeExecutions.pollFirst()
                val streamKey = pending?.first ?: "gemini_code_execution_${UUID.randomUUID()}"
                emitTracked(
                    result.toCodeExecutionCompletion(
                        streamKey = streamKey,
                        arguments = pending?.second ?: "{}",
                    ),
                )
            }
            part.functionCall?.let { fc ->
                val callId = fc.id?.takeIf(String::isNotBlank) ?: "call_${UUID.randomUUID()}"
                val argsJson = fc.args?.let {
                    traceJson.encodeToString(JsonObject.serializer(), it)
                } ?: "{}"
                val signature = partThoughtSignature
                    ?: fc.thoughtSignature?.takeIf(String::isNotBlank)
                    ?: currentThoughtSignature
                val streamKey = "call_stream_${UUID.randomUUID()}"
                if (
                    fc.name.matches(safeWireToolName) &&
                    callId.matches(safeWireToolCallId) &&
                    completedToolCallIds.add(callId)
                ) {
                    emitTracked(
                        StreamEvent.ToolCallUpdate(streamKey, callId, fc.name, argsJson, signature),
                    )
                    emitTracked(
                        StreamEvent.ToolCallRequest(
                            callId, fc.name, argsJson, signature, streamKey,
                        ),
                    )
                } else {
                    emitTracked(
                        malformedToolCallRequest(
                            cause = "Gemini returned invalid or duplicate tool metadata",
                            originalName = fc.name,
                            originalArguments = argsJson,
                            streamKey = streamKey,
                            signature = signature,
                        ),
                    )
                }
                currentThoughtSignature = null
                inThoughtBlock = false
            }
        }
        candidate?.groundingMetadata
            ?.toGeminiCitations(answerText.toString())
            ?.forEach { citation -> emitTracked(StreamEvent.CitationUpdate(citation)) }
        response.usageMetadata?.let { emitTracked(StreamEvent.UsageUpdate(it.toTokenUsage())) }
        if (streamError != null || finishReason != null) stopped = true
    }

    // ---- after the loop: termination decides about the final citation pass ----------------------
    val termination = geminiStreamTermination(
        sawDone,
        finishReason,
        producedContent,
        streamError,
        timedOut = false,
        toolCallInFlight = toolCallInFlight || pendingCodeExecutions.isNotEmpty(),
    )
    if (!termination.isRetryable) {
        latestGroundingMetadata
            ?.toGeminiCitations(answerText.toString())
            ?.forEach { citation -> emitTracked(StreamEvent.CitationUpdate(citation)) }
    }

    line(
        "term",
        "sawDone=${if (termination.sawTerminalMarker) 1 else 0}",
        "finish=${termination.stopReason?.let { esc(it) } ?: "-"}",
        "produced=${if (termination.producedContent) 1 else 0}",
        "inFlight=${if (termination.toolCallInFlight) 1 else 0}",
    )
    val terminatedError = termination.streamError
    line(
        "streamError",
        when (terminatedError) {
            null -> "none"
            is com.newoether.agora.api.GenerationError.Api ->
                "api|code=${terminatedError.code?.let { esc(it) } ?: "-"}" +
                    "|type=${terminatedError.type?.let { esc(it) } ?: "-"}" +
                    "|message=${esc(terminatedError.message)}"
            is com.newoether.agora.api.GenerationError.SseParse ->
                "sseParse|raw=${esc(terminatedError.rawLine)}"
            else -> error("unexpected stream error: ${terminatedError::class.simpleName}")
        },
    )
    print(out)
}