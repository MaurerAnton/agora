// Golden trace driver for the OpenAI chat SSE consumer, used to pin the C++ port.
//
// Runs the REAL Kotlin consumer (OpenAiChatStreamConsumer, compiled from the app sources) over a
// canned transcript of SSE lines / read timeouts / read errors, and prints a canonical trace.
// The C++ core must reproduce the same trace byte for byte for every transcript.
//
// Transcript format (one op per line, verbatim payloads, '#' comments and blank lines ignored):
//   L <line...>     feed one raw stream line
//   T               a read timed out
//   R <message...>  a response-body read failure surfaced with this message, then stop
//   E               end of stream (EOF)
//
// Build/run: see core/tools/golden/openai/run_golden.sh
package openai.golden

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.openai.OpenAiChatStreamConsumer
import com.newoether.agora.model.TokenUsage
import kotlinx.serialization.json.Json
import java.io.File
import kotlin.coroutines.Continuation
import kotlin.coroutines.EmptyCoroutineContext
import kotlin.coroutines.startCoroutine

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|[0-9a-f]{32}"
)

/** Random UUIDs are not comparable across implementations; collapse them to a marker. */
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

private fun orNull(value: String?): String = value?.let(::esc) ?: "-"
private fun orNullInt(value: Int?): String = value?.toString() ?: "-"
private fun flag(value: Boolean): String = if (value) "1" else "0"

private fun callLine(call: StreamEvent.ToolCallRequest): String =
    "id=" + norm(call.id) + "|name=" + esc(call.name) + "|args=" + esc(call.arguments) +
        "|streamKey=" + norm(call.streamKey)

private fun usageLine(usage: TokenUsage): String =
    "usage|total=${usage.totalTokenCount}|input=${orNullInt(usage.inputTokenCount)}" +
        "|cached=${orNullInt(usage.cachedInputTokenCount)}" +
        "|cacheWrite=${orNullInt(usage.cacheWriteInputTokenCount)}" +
        "|uncached=${orNullInt(usage.uncachedInputTokenCount)}" +
        "|output=${orNullInt(usage.outputTokenCount)}" +
        "|reasoning=${orNullInt(usage.reasoningTokenCount)}"

private fun eventLine(event: StreamEvent): String = when (event) {
    is StreamEvent.TextChunk -> "text|" + esc(event.text)
    is StreamEvent.ThoughtChunk ->
        "thought|" + esc(event.thought) + "|title=" + orNull(event.title) +
            "|signature=" + orNull(event.signature)
    is StreamEvent.ToolCallUpdate ->
        "toolCallUpdate|streamKey=" + norm(event.streamKey) + "|id=" +
            (event.id?.let(::norm) ?: "-") + "|name=" + esc(event.name) +
            "|args=" + esc(event.arguments)
    is StreamEvent.ToolCallRequest -> "toolCall|" + callLine(event)
    is StreamEvent.ToolCallsRequest -> "toolCalls|" + event.calls.joinToString(";;") { callLine(it) }
    is StreamEvent.UsageUpdate -> usageLine(event.usage)
    is StreamEvent.CitationUpdate -> "citation|"
    is StreamEvent.HostedToolCallUpdate -> "hostedToolCall|"
    is StreamEvent.Retrying -> "retrying|attempt=${event.attempt}|max=${event.maxAttempts}"
    is StreamEvent.Error -> "error|" + errorLine(event.error)
}

/** Error identity without implementation-specific cause texts (they differ by design). */
private fun errorLine(error: GenerationError?): String = when (error) {
    null -> "none"
    is GenerationError.Network -> "network|code=${error.statusCode}|message=" + esc(error.message)
    is GenerationError.Api ->
        "api|code=" + orNull(error.code) + "|type=" + orNull(error.type) +
            "|message=" + esc(error.message)
    is GenerationError.SseParse -> "sseParse|raw=" + esc(error.rawLine)
    is GenerationError.IncompleteStream ->
        "incomplete|provider=" + esc(error.provider) + "|stop=" + orNull(error.stopReason) +
            "|inFlight=" + flag(error.toolCallInFlight) + "|produced=" + flag(error.producedContent)
    is GenerationError.OutputTruncated ->
        "outputTruncated|provider=" + esc(error.provider) + "|stop=" + orNull(error.stopReason)
    is GenerationError.MalformedToolCall -> "malformedToolCall|cause=" + esc(error.cause)
    else -> error::class.simpleName ?: "unknown"
}

/** The emission lambda never actually suspends, so a synchronous continuation resumes inline. */
private fun <T> runSync(block: suspend () -> T): T {
    var result: Result<T>? = null
    block.startCoroutine(Continuation(EmptyCoroutineContext) { result = it })
    return checkNotNull(result) { "suspend block did not resume synchronously" }.getOrThrow()
}

fun main(args: Array<String>) {
    require(args.size == 1) { "usage: TraceDump <transcript.txt>" }
    val ops = File(args[0]).readLines()
        .map { it.trimEnd('\r') }
        .filter { it.isNotBlank() && !it.startsWith("#") }

    val json = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }
    val consumer = OpenAiChatStreamConsumer(json = json, providerName = "agora")

    val out = StringBuilder()
    fun record(event: StreamEvent) {
        out.append(eventLine(event)).append('\n')
    }

    var stopped = false
    var terminalActive = false
    for (op in ops) {
        if (stopped) break
        when {
            op == "T" -> {
                if (!consumer.onReadTimeout(terminalActive)) stopped = true
            }
            op == "E" -> stopped = true
            op.startsWith("L ") -> {
                val line = op.substring(2)
                val keepReading = runSync { consumer.consumeLine(line) { event -> record(event) } }
                if (!keepReading) {
                    stopped = true
                } else if (!terminalActive && consumer.hasSemanticTerminal) {
                    terminalActive = true
                }
            }
            op.startsWith("R ") -> {
                consumer.onReadError(
                    GenerationError.Network(statusCode = 0, message = op.substring(2))
                )
                stopped = true
            }
            else -> error("Unknown op: $op")
        }
    }

    val termination = consumer.finish()
    out.append(
        "term|sawTerminal=" + flag(termination.sawTerminalMarker) +
            "|stop=" + orNull(termination.stopReason) +
            "|produced=" + flag(termination.producedContent) +
            "|inFlight=" + flag(termination.toolCallInFlight) +
            "|timedOut=" + flag(termination.timedOut) +
            "|reported=" + flag(termination.alreadyReportedError) +
            "|retryable=" + flag(termination.isRetryable) +
            "|truncCap=" + flag(termination.truncatedByTokenCap) + "\n"
    )
    out.append("streamError|" + errorLine(termination.streamError) + "\n")
    out.append("toError|" + errorLine(termination.toError("agora")) + "\n")
    out.append("describe|" + esc(termination.describe()) + "\n")

    print(out)
}
