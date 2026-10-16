// Golden trace driver for the stream post-processing pipeline, used to pin the C++ port.
//
// Runs the REAL Kotlin sources (ProviderStreamNormalizer + the incremental thinking lexer + the
// text tool-call parsers + StreamTermination + ToolArgumentAccumulator + MalformedToolCall,
// compiled straight from the app tree) over a canned JSON scenario and prints a canonical trace.
// The C++ core must reproduce the same trace byte for byte.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package stream.normalization.golden

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.openai.StreamingTextToolCallParser
import com.newoether.agora.api.openai.ToolCallTextParser
import com.newoether.agora.api.util.IncrementalThinkingParser
import com.newoether.agora.api.util.ProviderRetryPolicy
import com.newoether.agora.api.util.ProviderStreamNormalizer
import com.newoether.agora.api.util.StreamTermination
import com.newoether.agora.api.util.ToolArgumentAccumulator
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.api.util.findImplicitThinkingClose
import com.newoether.agora.api.util.malformedToolCallRequest
import com.newoether.agora.api.util.malformedToolCallResultText
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

private val traceJson = Json { ignoreUnknownKeys = true }

/** Dashed UUIDs and the two compact-suffix families the malformed stand-in produces. */
private fun norm(value: String): String {
    val dashed = Regex("[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}")
    return value
        .replace(dashed) { "<uuid>" }
        .replace(Regex("call_malformed_[0-9a-f]{32}")) { "call_malformed_<uuid>" }
        .replace(Regex("malformed_tool_[0-9a-f]{32}")) { "malformed_tool_<uuid>" }
}

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

// -- scenario shape -----------------------------------------------------------------------------------

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "normalizer",
    /** normalizer: tool names offered to the stream. */
    val tools: List<String> = emptyList(),
    val nativeTextParsingAuthoritative: Boolean = false,
    val releaseTextTools: Boolean = true,
    val events: List<ScenarioEvent> = emptyList(),
    /** thinkingParser / textParser: raw chunks fed one by one. */
    val chunks: List<String> = emptyList(),
    val startInThinking: Boolean = false,
    /** thinkingParser: feed/flush thinking switches (pins the disabled branches). */
    val thinkingEnabled: Boolean = true,
    val flushThinkingEnabled: Boolean? = null,
    /** toolParser: content passed to ToolCallTextParser.parse. */
    val content: String = "",
    /** accumulator: fragments appended in order (null entries pin the null-fragment rule). */
    val fragments: List<String?> = emptyList(),
    /** closeBoundary: content scanned for the first implicit close marker. */
    val text: String = "",
    /** termination: the StreamTermination fields. */
    val sawTerminalMarker: Boolean = false,
    val stopReason: String? = null,
    val producedContent: Boolean = false,
    val toolCallInFlight: Boolean = false,
    val hasStreamError: Boolean = false,
    val streamErrorMessage: String = "",
    val alreadyReportedError: Boolean = false,
    val timedOut: Boolean = false,
    val retryableStreamError: Boolean = true,
    /** malformed: the stand-in inputs. */
    val cause: String = "",
    val originalName: String? = null,
    val originalArguments: String? = null,
    val streamKey: String? = null,
    val signature: String? = null,
    /** retry: policy probes. */
    val retryNumber: Int = 1,
    val statusCode: Int = 500,
    val body: String? = null,
    val retryableStatusCodes: List<Int> = listOf(429, 500, 503),
)

@Serializable
private data class ScenarioEvent(
    val type: String,
    val text: String = "",
    val thought: String = "",
    val title: String? = null,
    val signature: String? = null,
    val id: String? = null,
    val name: String = "",
    val arguments: String = "",
    val streamKey: String? = null,
    val calls: List<ScenarioEvent> = emptyList(),
    val tokenCount: Int = 0,
    val attempt: Int = 0,
    val maxAttempts: Int = 0,
    val isError: Boolean = false,
    val result: String? = null,
)

// -- event building -------------------------------------------------------------------------------------

private fun buildToolCall(event: ScenarioEvent): StreamEvent.ToolCallRequest =
    StreamEvent.ToolCallRequest(
        id = event.id ?: "",
        name = event.name,
        arguments = event.arguments,
        signature = event.signature,
        streamKey = event.streamKey ?: (event.id ?: ""),
    )

private fun buildEvent(event: ScenarioEvent): StreamEvent = when (event.type) {
    "text" -> StreamEvent.TextChunk(event.text)
    "thought" -> StreamEvent.ThoughtChunk(event.thought, event.title, event.signature)
    "toolCall" -> buildToolCall(event)
    "toolCalls" -> StreamEvent.ToolCallsRequest(event.calls.map(::buildToolCall))
    "error" -> StreamEvent.Error(GenerationError.SseParse("raw_line", "cause"))
    "usage" -> StreamEvent.UsageUpdate(event.tokenCount)
    "retrying" -> StreamEvent.Retrying(event.attempt, event.maxAttempts)
    "toolCallUpdate" -> StreamEvent.ToolCallUpdate(
        streamKey = event.streamKey ?: "",
        id = event.id,
        name = event.name,
        arguments = event.arguments,
        signature = event.signature,
    )
    "hosted" -> StreamEvent.HostedToolCallUpdate(
        streamKey = event.streamKey ?: "",
        name = event.name,
        arguments = event.arguments,
        result = event.result,
        isError = event.isError,
    )
    "citation" -> StreamEvent.CitationUpdate(
        com.newoether.agora.model.CitationRecord(sourceId = "s1", provider = "p", kind = "web", title = "T")
    )
    else -> error("unknown event type: ${event.type}")
}

// -- downstream trace lines --------------------------------------------------------------------------------

private fun eventLine(event: StreamEvent): String = when (event) {
    is StreamEvent.TextChunk -> "text|" + esc(event.text)
    is StreamEvent.ThoughtChunk ->
        "thought|" + esc(event.thought) + "|" + esc(event.title ?: "-") + "|" + esc(event.signature ?: "-")
    is StreamEvent.ToolCallRequest ->
        "toolCall|" + esc(norm(event.id)) + "|" + esc(event.name) + "|" + esc(event.arguments) +
            "|" + esc(event.signature ?: "-") + "|" + esc(norm(event.streamKey)) + "|" +
            event.responseOutputItems.size.toString()
    is StreamEvent.ToolCallsRequest ->
        "toolCalls|" + event.calls.size.toString() + "\n" +
            event.calls.joinToString("\n") { call -> eventLine(call) }
    is StreamEvent.Error -> "error|" + errorLine(event.error)
    is StreamEvent.UsageUpdate -> "usage|" + event.usage.totalTokenCount.toString() + "|" +
        (event.usage.reasoningTokenCount?.toString() ?: "-")
    is StreamEvent.Retrying -> "retrying|" + event.attempt.toString() + "|" + event.maxAttempts.toString()
    is StreamEvent.ToolCallUpdate ->
        "toolCallUpdate|" + esc(norm(event.streamKey)) + "|" + esc(norm(event.id ?: "-")) + "|" +
            esc(event.name) + "|" + esc(event.arguments) + "|" + esc(event.signature ?: "-")
    is StreamEvent.HostedToolCallUpdate ->
        "hosted|" + esc(norm(event.streamKey)) + "|" + esc(event.name) + "|" +
            esc(event.arguments) + "|" + esc(event.result ?: "-") + "|" +
            if (event.isError) "1" else "0"
    is StreamEvent.CitationUpdate -> "citation"
}

private fun errorLine(error: GenerationError): String = when (error) {
    is GenerationError.Network -> "network|" + error.statusCode.toString() + "|" + esc(error.message)
    is GenerationError.Api ->
        "api|" + esc(error.code ?: "-") + "|" + esc(error.type ?: "-") + "|" + esc(error.message)
    is GenerationError.SseParse -> "sseParse|" + esc(error.rawLine) + "|" + esc(error.cause)
    is GenerationError.MalformedToolCall -> "malformedToolCall|" + esc(error.cause)
    is GenerationError.IncompleteStream ->
        "incomplete|" + esc(error.provider) + "|" + esc(error.stopReason ?: "-") + "|" +
            (if (error.toolCallInFlight) "1" else "0") + "|" + (if (error.producedContent) "1" else "0")
    is GenerationError.OutputTruncated ->
        "outputTruncated|" + esc(error.provider) + "|" + esc(error.stopReason ?: "-")
    is GenerationError.Timeout -> "Timeout"
    else -> "other"
}

// -- main -----------------------------------------------------------------------------------------------

fun main(args: Array<String>) {
    require(args.size == 1) { "usage: StreamNormalizationTraceDump <scenario.json>" }
    val scenario = traceJson.decodeFromString(Scenario.serializer(), File(args[0]).readText())
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }
    fun record(event: StreamEvent) {
        out.append(eventLine(event)).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    // The parsers are suspend APIs; the reference driver hosts them in one coroutine.
    try {
        runBlocking {
            when (scenario.mode) {
            "normalizer" -> {
                val normalizer = ProviderStreamNormalizer(
                    tools = scenario.tools.map { name ->
                        com.newoether.agora.api.ToolDefinition(
                            function = com.newoether.agora.api.ToolFunction(
                                name = name,
                                description = "",
                                parameters = com.newoether.agora.api.ToolParameters(properties = emptyMap()),
                            ),
                        )
                    },
                    nativeTextParsingAuthoritative = scenario.nativeTextParsingAuthoritative,
                )
                val downstream: suspend (StreamEvent) -> Unit = { event -> record(event) }
                scenario.events.forEach { event -> normalizer.emit(buildEvent(event), downstream) }
                normalizer.finish(downstream, releaseTextTools = scenario.releaseTextTools)
            }
            "thinkingParser" -> {
                val parser = IncrementalThinkingParser(startInThinking = scenario.startInThinking)
                val pendingText = StringBuilder()
                val pendingThought = StringBuilder()
                val onText: suspend (String) -> Unit = { text -> pendingText.append(text) }
                val onThought: suspend (String) -> Unit = { thought -> pendingThought.append(thought) }
                scenario.chunks.forEachIndexed { index, chunk ->
                    pendingText.setLength(0)
                    pendingThought.setLength(0)
                    parser.feed(
                        content = chunk,
                        thinkingEnabled = scenario.thinkingEnabled,
                        onText = onText,
                        onThought = onThought,
                    )
                    line("chunk", index.toString())
                    if (pendingText.isNotEmpty()) line("text", esc(pendingText.toString()))
                    if (pendingThought.isNotEmpty()) line("thought", esc(pendingThought.toString()))
                    line("pending", esc(parser.pendingContent))
                }
                pendingText.setLength(0)
                pendingThought.setLength(0)
                if (scenario.flushThinkingEnabled == null) {
                    parser.flush(onText = onText, onThought = onThought)
                } else {
                    parser.flush(thinkingEnabled = scenario.flushThinkingEnabled, onText = onText, onThought = onThought)
                }
                out.append("flush\n")
                if (pendingText.isNotEmpty()) line("text", esc(pendingText.toString()))
                if (pendingThought.isNotEmpty()) line("thought", esc(pendingThought.toString()))
                line("pending", esc(parser.pendingContent))
                line("inThinking", if (parser.inThinking) "1" else "0")
            }
            "textParser" -> {
                val parser = StreamingTextToolCallParser()
                val onText: suspend (String) -> Unit = { text -> line("text", esc(text)) }
                val onUpdate: suspend (StreamingTextToolCallParser.Snapshot) -> Unit = { snapshot ->
                    line("update", esc(norm(snapshot.streamKey)), esc(snapshot.name), esc(snapshot.arguments))
                }
                val onComplete: suspend (StreamingTextToolCallParser.CompletedCall) -> Unit = { call ->
                    line(
                        "complete",
                        esc(norm(call.streamKey)),
                        esc(norm(call.id ?: "-")),
                        esc(call.name),
                        esc(call.arguments),
                    )
                }
                val onMalformed: suspend (String) -> Unit = { cause -> line("malformed", esc(cause)) }
                // Chunk-by-chunk feeding pins the boundary behavior exactly like the app's stream.
                scenario.chunks.forEach { chunk ->
                    parser.feed(
                        content = chunk,
                        onText = onText,
                        onUpdate = onUpdate,
                        onComplete = onComplete,
                        onMalformed = onMalformed,
                    )
                }
                parser.flush(
                    onText = onText,
                    onUpdate = onUpdate,
                    onComplete = onComplete,
                    onMalformed = onMalformed,
                )
            }
            "toolParser" -> {
                val calls = ToolCallTextParser.parse(scenario.content)
                calls.forEach { call ->
                    line(
                        "call",
                        esc(call.id ?: "-"),
                        esc(call.name),
                        esc(call.arguments),
                    )
                }
                if (calls.isEmpty()) line("calls", "0")
            }
            "accumulator" -> {
                val accumulator = ToolArgumentAccumulator()
                scenario.fragments.forEach { fragment -> accumulator.append(fragment) }
                line("value", esc(accumulator.toString()))
                line("empty", if (accumulator.isEmpty) "1" else "0")
            }
            "closeBoundary" -> {
                val boundary = findImplicitThinkingClose(scenario.text)
                if (boundary == null) {
                    line("boundary", "none")
                } else {
                    line("boundary", boundary.startIndex.toString(), boundary.endExclusive.toString())
                }
            }
            "termination" -> {
                val termination = StreamTermination(
                    sawTerminalMarker = scenario.sawTerminalMarker,
                    stopReason = scenario.stopReason,
                    producedContent = scenario.producedContent,
                    toolCallInFlight = scenario.toolCallInFlight,
                    streamError = if (scenario.hasStreamError) {
                        GenerationError.Api(code = null, type = "relay", message = scenario.streamErrorMessage)
                    } else {
                        null
                    },
                    alreadyReportedError = scenario.alreadyReportedError,
                    timedOut = scenario.timedOut,
                    retryableStreamError = scenario.retryableStreamError,
                )
                line("truncated", if (termination.truncatedByTokenCap) "1" else "0")
                line("retryable", if (termination.isRetryable) "1" else "0")
                val error = termination.toError("OpenAI")
                line("error", if (error == null) "none" else errorLine(error))
            }
            "malformed" -> {
                val event = malformedToolCallRequest(
                    cause = scenario.cause,
                    originalName = scenario.originalName,
                    originalArguments = scenario.originalArguments,
                    streamKey = scenario.streamKey,
                    signature = scenario.signature,
                )
                line(
                    "event",
                    esc(norm(event.id)),
                    esc(event.name),
                    esc(event.arguments),
                    esc(event.signature ?: "-"),
                    esc(norm(event.streamKey)),
                )
                line("result", esc(malformedToolCallResultText(event.arguments)))
            }
            "carriesOutput" -> line("output", if (buildEvent(scenario.events.first()).carriesModelOutput()) "1" else "0")
            "retry" -> {
                line("delay", ProviderRetryPolicy.delayMillis(scenario.retryNumber).toString())
                val body = scenario.body
                line("upstreamFailure", if (ProviderRetryPolicy.isRetryableUpstreamFailure(body)) "1" else "0")
                val codes = scenario.retryableStatusCodes.toSet()
                line("retryHttp", if (ProviderRetryPolicy.shouldRetryHttp(scenario.statusCode, body, codes)) "1" else "0")
                val streamError: GenerationError =
                    GenerationError.Api(code = null, type = "relay", message = scenario.streamErrorMessage)
                line("streamRetry", if (ProviderRetryPolicy.shouldRetryStreamError(streamError)) "1" else "0")
            }
            else -> error("unknown mode: ${scenario.mode}")
            }
        }
    } catch (error: Exception) {
        // No format gate in this module: the exception class alone is part of the contract.
        line("error", error.javaClass.simpleName ?: "unknown")
    }
    print(out)
}
