// Golden trace driver for the Anthropic stream event router, used to pin the C++ port.
//
// Runs the REAL Kotlin router (AnthropicStreamEventRouter, compiled from the app sources) over a
// canned transcript that replays the semantic half of the provider's read loop: every `data:`
// line is decoded with the provider's exact Json configuration and routed, and the loop's
// stopping conditions are evaluated exactly as AnthropicProvider does. The transport half
// (socket reads, read timeouts, cancellation, HTTP error statuses) is out of scope and not part
// of this stand's contract.
//
// Transcript format (one op per line, '#' comments and blank lines ignored):
//   L <line...>     feed one raw stream line through the loop; ignored once the loop stopped
//   C <raw...>      call captureParseError(raw, "driver") directly, then stop the loop (the
//                   decode-catch path is the only caller in the provider)
//   I               call reportIncompleteBlocks() directly (the provider calls it once after the
//                   loop; this op pins repeated calls mid-transcript)
//   E               stop reading (end of stream)
//
// The outer reportIncompleteBlocks() call after the loop always runs, like in the provider.
//
// Build/run: see core/tools/golden/anthropic-stream/run_golden.sh
package anthropicstream.golden

import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.anthropic.AnthropicStreamEvent
import com.newoether.agora.api.anthropic.AnthropicStreamEventRouter
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.model.CitationAnchor
import com.newoether.agora.model.CitationRecord
import kotlinx.serialization.json.Json
import java.io.File

/** The provider's own Json instance, config for config (AnthropicProvider.kt). */
private val providerJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

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
            '"' -> append("\\q")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(c)
        }
    }
}

private fun q(value: String): String = "\"" + esc(value) + "\""
private fun qn(value: String?): String = value?.let(::q) ?: "null"
private fun orNull(value: String?): String = value?.let(::esc) ?: "-"
private fun orNullInt(value: Int?): String = value?.toString() ?: "-"
private fun flag(value: Boolean): String = if (value) "1" else "0"

// -- canonical record / error / usage rendering -------------------------------------------------------

private fun anchorField(anchor: CitationAnchor): String =
    "{" + anchor.startIndex + "," + anchor.endIndex + "," + q(anchor.citedText) + "}"

private fun recordFields(record: CitationRecord): String = listOf(
    "v=" + record.version,
    "sourceId=" + q(record.sourceId),
    "provider=" + q(record.provider),
    "kind=" + q(record.kind),
    "title=" + q(record.title),
    "url=" + qn(record.url),
    "fileName=" + qn(record.fileName),
    "location=" + qn(record.location),
    "providerSourceId=" + qn(record.providerSourceId),
    "excerpt=" + qn(record.excerpt),
    "anchors=[" + record.anchors.joinToString(",") { anchorField(it) } + "]",
).joinToString(";")

/** Error identity without implementation-specific cause texts (they differ by design). */
private fun errorLine(error: GenerationError?): String = when (error) {
    null -> "none"
    is GenerationError.Api ->
        "api|code=" + orNull(error.code) + "|type=" + orNull(error.type) +
            "|message=" + esc(error.message)
    is GenerationError.SseParse -> "sseParse|raw=" + esc(error.rawLine)
    else -> error::class.simpleName ?: "unknown"
}

private fun usageLine(usage: com.newoether.agora.model.TokenUsage): String =
    "usage|total=${usage.totalTokenCount}|input=${orNullInt(usage.inputTokenCount)}" +
        "|cached=${orNullInt(usage.cachedInputTokenCount)}" +
        "|cacheWrite=${orNullInt(usage.cacheWriteInputTokenCount)}" +
        "|uncached=${orNullInt(usage.uncachedInputTokenCount)}" +
        "|output=${orNullInt(usage.outputTokenCount)}" +
        "|reasoning=${orNullInt(usage.reasoningTokenCount)}"

private fun callLine(call: StreamEvent.ToolCallRequest): String =
    "id=" + norm(call.id) + "|name=" + esc(call.name) + "|args=" + esc(call.arguments) +
        "|streamKey=" + norm(call.streamKey)

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
    is StreamEvent.UsageUpdate -> usageLine(event.usage)
    is StreamEvent.CitationUpdate -> "citation|" + recordFields(event.citation)
    is StreamEvent.Error -> "error|" + errorLine(event.error)
    // The router never emits the remaining kinds; render them by name so drift is visible.
    else -> "unexpected|" + event::class.simpleName
}

// -- main -----------------------------------------------------------------------------------------------

fun main(args: Array<String>) {
    require(args.size == 1) { "usage: AnthropicStreamTraceDump <transcript.txt>" }
    val ops = File(args[0]).readLines()
        .map { it.trimEnd('\r') }
        .filter { it.isNotBlank() && !it.startsWith("#") }

    val out = StringBuilder()
    fun record(event: StreamEvent) {
        out.append(eventLine(event)).append('\n')
    }
    val router = AnthropicStreamEventRouter()
    var producedContent = false
    var driverReported = false
    fun emitTracked(event: StreamEvent) {
        if (event.carriesModelOutput()) producedContent = true
        if (event is StreamEvent.Error) driverReported = true
        record(event)
    }

    var stopped = false
    for (op in ops) {
        if (op == "E") {
            stopped = true
            continue
        }
        if (op == "I") {
            router.reportIncompleteBlocks().forEach(::emitTracked)
            continue
        }
        if (op.startsWith("C ")) {
            router.captureParseError(rawLine = op.substring(2), cause = "driver")
            stopped = true
            continue
        }
        if (op.startsWith("L ")) {
            if (stopped) continue
            val line = op.substring(2)
            if (line.startsWith("data: ")) {
                val jsonStr = line.substring(6).trim()
                try {
                    val event = providerJson.decodeFromString<AnthropicStreamEvent>(jsonStr)
                    router.route(event).forEach(::emitTracked)
                } catch (e: Exception) {
                    router.captureParseError(
                        rawLine = jsonStr,
                        cause = e.localizedMessage ?: "Malformed SSE payload",
                    )
                    stopped = true
                }
            }
            if (router.streamError != null || router.reportedError || driverReported) stopped = true
            if (router.messageStopReceived) stopped = true
            continue
        }
        error("Unknown op: $op")
    }

    router.reportIncompleteBlocks().forEach(::emitTracked)
    out.append(
        "term|messageStop=" + flag(router.messageStopReceived) +
            "|stop=" + orNull(router.stopReason) +
            "|toolUseStarts=" + router.toolUseBlockStarts +
            "|sawTerminal=" + flag(router.sawTerminalMarker) +
            "|inFlight=" + flag(router.toolCallInFlight) +
            "|reported=" + flag(router.reportedError) +
            "|produced=" + flag(producedContent) +
            "|driverReported=" + flag(driverReported) + "\n"
    )
    out.append("streamError|" + errorLine(router.streamError) + "\n")

    print(out)
}
