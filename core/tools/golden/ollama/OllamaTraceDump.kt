// Golden trace driver for the Ollama request pipeline and stream loop, used to pin the C++ port.
//
// Runs the REAL Kotlin declarations (the wire models and loop helpers extracted verbatim from
// OllamaProvider.kt, plus the app's validators, converters and registries) over canned JSON
// scenarios and prints a canonical trace. The C++ core must reproduce the same trace byte for
// byte. The provider's local functions (buildApiMessages, the options/think assembly, buildRequestBody,
// the NDJSON loop) are transcriptions of OllamaProvider.kt — the traces pin every line.
//
// Modes: factory | validate | serialized | stream (the stream mode replays `ops`, raw NDJSON lines).
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package ollama.golden

import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.ProviderRequestInput
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty
import com.newoether.agora.api.ollama.OllamaChatRequest
import com.newoether.agora.api.ollama.OllamaMessage
import com.newoether.agora.api.ollama.OllamaStreamResponse
import com.newoether.agora.api.ollama.ollamaStreamTermination
import com.newoether.agora.api.ollama.requireValidWireFormat
import com.newoether.agora.api.ollama.toTokenUsage
import com.newoether.agora.api.util.Base64FileRegistry
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.util.StreamingJsonBody
import com.newoether.agora.api.util.buildToolCallId
import com.newoether.agora.api.util.malformedToolCallRequest
import com.newoether.agora.api.util.requireValidSerializedRequest
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.api.util.safeWireToolCallId
import com.newoether.agora.api.util.safeWireToolName
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.Participant
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.model.ToolCallData
import com.newoether.agora.util.Constants
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.MapSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Path
import java.util.Base64
import java.util.UUID

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|[0-9a-f]{32}",
)

/** Random UUIDs (stream keys, malformed stand-in ids) collapse, dashed and compact alike. */
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

/**
 * The exact bytes the wire body produces once every placeholder is replaced by its base64: the
 * same substitution ReplayableJsonRequestBody.writeTo performs, without the transport adapter.
 */
private fun resolvedBodyText(body: StreamingJsonBody): String {
    val out = ByteArrayOutputStream()
    out.write(body.segments[0])
    body.parts.forEachIndexed { index, part ->
        out.write(Base64.getEncoder().encode(part.file.readBytes()))
        out.write(body.segments[index + 1])
    }
    return out.toString(Charsets.UTF_8.name())
}

private fun relPath(filesRoot: Path, file: File): String =
    filesRoot.relativize(file.toPath()).toString()

/** Scenario floats may spell the values JSON cannot carry ("NaN", "Infinity", "-Infinity"). */
private fun scenarioFloat(primitive: JsonPrimitive): Float = when (val text = primitive.content) {
    "NaN" -> Float.NaN
    "Infinity" -> Float.POSITIVE_INFINITY
    "-Infinity" -> Float.NEGATIVE_INFINITY
    else -> text.toFloat()
}

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "factory",
    val config: ScenarioConfig = ScenarioConfig(),
    val requestInput: ScenarioInput? = null,
    val request: JsonObject? = null,
    val serializedBody: String? = null,
    val serializedRequiredStringFields: List<String> = emptyList(),
    val serializedRequiredArrayFields: List<String> = emptyList(),
    val ops: List<String> = emptyList(),
)

@Serializable
private data class ScenarioConfig(
    val modelId: String = "llama3.2:3b",
    val thinkingEnabled: Boolean = true,
    val thinkingLevel: String = "medium",
    val thinkingBudgetEnabled: Boolean = false,
    val thinkingBudgetTokens: Int = 4096,
    val includeImages: Boolean = true,
    val temperature: JsonPrimitive? = null,
    val maxTokens: Int? = null,
    val topP: JsonPrimitive? = null,
    val tools: List<JsonObject>? = null,
)

@Serializable
private data class ScenarioInput(
    val systemPrompt: String? = null,
    val messages: List<ScenarioMessage> = emptyList(),
)

@Serializable
private data class ScenarioMessage(
    val id: String,
    val participant: String,
    val text: String = "",
    val images: List<String> = emptyList(),
    val modelName: String? = null,
    val segments: List<ScenarioSegment>? = null,
    val toolCall: ScenarioToolCall? = null,
)

@Serializable
private data class ScenarioSegment(
    val type: String,
    val content: String = "",
    val toolName: String? = null,
    val toolArgs: String? = null,
    val toolResult: String? = null,
    val toolCallId: String? = null,
    val signature: String? = null,
    val signatureProvider: String? = null,
    val responseOutputItems: List<JsonObject> = emptyList(),
    val responseOutputItemProvider: String? = null,
)

@Serializable
private data class ScenarioToolCall(
    val toolName: String,
    val arguments: String,
    val result: String,
    val toolCallId: String? = null,
    val signature: String? = null,
    val responseOutputItems: List<JsonObject> = emptyList(),
    val responseOutputItemProvider: String? = null,
)

private fun resolveMedia(filesRoot: File, reference: String): File =
    File(filesRoot, reference).canonicalFile

private fun buildMessage(filesRoot: File, message: ScenarioMessage): ChatMessage = ChatMessage(
    id = message.id,
    text = message.text,
    images = message.images.map { reference -> resolveMedia(filesRoot, reference).absolutePath },
    participant = when (message.participant) {
        "USER" -> Participant.USER
        "MODEL" -> Participant.MODEL
        "ERROR" -> Participant.ERROR
        else -> error("unknown participant: ${message.participant}")
    },
    modelName = message.modelName,
    segments = message.segments?.map { segment ->
        MessageSegment(
            type = segment.type,
            content = segment.content,
            toolName = segment.toolName,
            toolArgs = segment.toolArgs,
            toolResult = segment.toolResult,
            toolCallId = segment.toolCallId,
            signature = segment.signature,
            signatureProvider = segment.signatureProvider,
            responseOutputItems = segment.responseOutputItems,
            responseOutputItemProvider = segment.responseOutputItemProvider,
        )
    },
    toolCall = message.toolCall?.let { call ->
        ToolCallData(
            toolName = call.toolName,
            arguments = call.arguments,
            result = call.result,
            toolCallId = call.toolCallId,
            signature = call.signature,
            responseOutputItems = call.responseOutputItems,
            responseOutputItemProvider = call.responseOutputItemProvider,
        )
    },
)

private fun buildTools(rawTools: List<JsonObject>?): List<ToolDefinition>? = rawTools?.map { raw ->
    val functionJson = raw["function"]?.jsonObject ?: error("tool without function")
    ToolDefinition(
        type = raw["type"]?.jsonPrimitive?.content ?: "function",
        function = ToolFunction(
            name = functionJson["name"]?.jsonPrimitive?.content ?: error("tool without name"),
            description = functionJson["description"]?.jsonPrimitive?.content ?: "",
            parameters = buildToolParameters(functionJson["parameters"]?.jsonObject),
        ),
    )
}

private fun buildToolParameters(json: JsonObject?): ToolParameters {
    if (json == null) return ToolParameters(properties = emptyMap())
    val properties = json["properties"]?.jsonObject?.let { rawProperties ->
        traceJson.decodeFromJsonElement(
            MapSerializer(String.serializer(), ToolProperty.serializer()),
            rawProperties,
        )
    } ?: emptyMap()
    val required = json["required"]?.jsonArray?.map { it.jsonPrimitive.content } ?: emptyList()
    return ToolParameters(
        type = json["type"]?.jsonPrimitive?.content ?: "object",
        properties = properties,
        required = required,
        schema = json["schema"]?.jsonObject,
    )
}

// ---- Transcribed from OllamaProvider.kt (local functions inside generateResponse) ----------------

/** OllamaProvider.buildApiMessages. */
private fun buildApiMessages(
    config: ProviderConfig,
    resolvedRequest: ProviderRequestInput,
    base64Files: Base64FileRegistry,
): List<OllamaMessage> {
    val apiMessages = mutableListOf<OllamaMessage>()
    if (!resolvedRequest.systemPrompt.isNullOrBlank()) {
        apiMessages.add(OllamaMessage(role = "system", content = resolvedRequest.systemPrompt))
    }

    apiMessages.addAll(resolvedRequest.messages.flatMap { msg ->
        val entries = mutableListOf<OllamaMessage>()

        // tool_ messages: assistant turn with tool_calls (and thinking from segments)
        if (msg.id.startsWith(Constants.TOOL_MSG_PREFIX)) {
            val toolSegs = msg.segments?.filter { it.type == "tool" }
            val thinkingContent = msg.segments?.lastOrNull { it.type == "thought" }?.content
            if (!toolSegs.isNullOrEmpty()) {
                val toolCalls = toolSegs.map { seg ->
                    val tid = seg.toolCallId ?: buildToolCallId(seg.toolName ?: "", seg.toolArgs ?: "{}")
                    val argsObj = try { traceJson.parseToJsonElement(seg.toolArgs ?: "{}") as? JsonObject } catch (_: Exception) { JsonObject(emptyMap()) }
                    com.newoether.agora.api.OpenAiToolCall(
                        id = tid,
                        type = "function",
                        function = com.newoether.agora.api.OpenAiFunctionCall(name = seg.toolName ?: "", arguments = argsObj ?: JsonObject(emptyMap()))
                    )
                }
                entries.add(OllamaMessage(
                    role = "assistant",
                    content = "",
                    thinking = thinkingContent?.ifEmpty { null },
                    toolCalls = toolCalls
                ))
            } else if (msg.toolCall != null) {
                val toolId = msg.toolCall!!.toolCallId ?: buildToolCallId(msg.toolCall!!.toolName, msg.toolCall!!.arguments)
                val argsObj = try { traceJson.parseToJsonElement(msg.toolCall!!.arguments) as? JsonObject } catch (_: Exception) { JsonObject(emptyMap()) }
                entries.add(OllamaMessage(
                    role = "assistant",
                    content = "",
                    thinking = thinkingContent?.ifEmpty { null },
                    toolCalls = listOf(com.newoether.agora.api.OpenAiToolCall(
                        id = toolId,
                        type = "function",
                        function = com.newoether.agora.api.OpenAiFunctionCall(name = msg.toolCall!!.toolName, arguments = argsObj ?: JsonObject(emptyMap()))
                    ))
                ))
            }
            return@flatMap entries
        }

        // result_ messages carry the tool result(s)
        if (msg.id.startsWith(Constants.RESULT_MSG_PREFIX)) {
            val toolSegs = msg.segments?.filter { it.type == "tool" }
            if (!toolSegs.isNullOrEmpty()) {
                for (seg in toolSegs) {
                    entries.add(OllamaMessage(
                        role = "tool",
                        content = seg.toolResult ?: "",
                        toolName = seg.toolName,
                    ))
                }
            } else if (msg.toolCall != null) {
                entries.add(OllamaMessage(
                    role = "tool",
                    content = msg.toolCall!!.result,
                    toolName = msg.toolCall!!.toolName,
                ))
            }
            return@flatMap entries
        }

        val images = if (config.includeImages && msg.participant == Participant.USER) {
            msg.images.mapNotNull(base64Files::register)
        } else null

        // Normal message: text + images only
        entries.add(OllamaMessage(
            role = if (msg.participant == Participant.USER) "user" else "assistant",
            content = msg.text,
            images = images?.takeIf { it.isNotEmpty() }
        ))
        entries
    })
    return apiMessages
}

internal data class BuiltOllamaRequest(
    val apiMessages: List<OllamaMessage>,
    val bodyJson: String,
    val streamingBody: StreamingJsonBody?
)

/** The provider's generateResponse preamble + buildRequestBody, in the provider's order. */
private fun buildRequestBody(
    config: ProviderConfig,
    input: ProviderRequestInput,
    base64Files: Base64FileRegistry,
): BuiltOllamaRequest {
    val options = buildMap<String, JsonElement> {
        config.temperature?.let { put("temperature", JsonPrimitive(it)) }
        config.topP?.let { put("top_p", JsonPrimitive(it)) }
        config.maxTokens?.let { put("num_predict", JsonPrimitive(it)) }
    }.takeIf { it.isNotEmpty() }?.let { JsonObject(it) }

    val resolvedThinking = config.resolvedThinking(ThinkingProviderFamily.OLLAMA)
    val think = resolvedThinking.effort
        ?.let { JsonPrimitive(it) }
        ?: JsonPrimitive(resolvedThinking.enabled)

    val requestBody = OllamaChatRequest(
        model = config.modelId,
        messages = buildApiMessages(config, input, base64Files),
        stream = true,
        options = options,
        tools = config.tools,
        think = think,
    )

    requestBody.requireValidWireFormat()
    val requestBodyJson = traceJson.encodeToString(OllamaChatRequest.serializer(), requestBody)
    val streamingRequest = base64Files.prepare(requestBodyJson)
    requireValidSerializedRequest(
        provider = "Ollama",
        body = requestBodyJson,
        requiredStringFields = setOf("model"),
        requiredArrayFields = setOf("messages"),
    )
    return BuiltOllamaRequest(requestBody.messages, requestBodyJson, streamingRequest)
}

// ---- Trace rendering -----------------------------------------------------------------------------

private fun orNull(value: String?): String = value?.let { esc(it) } ?: "-"

private fun usageLine(usage: com.newoether.agora.model.TokenUsage): String =
    "usage|total=${usage.totalTokenCount}" +
        "|input=${usage.inputTokenCount ?: "-"}" +
        "|cached=${usage.cachedInputTokenCount ?: "-"}" +
        "|cacheWrite=${usage.cacheWriteInputTokenCount ?: "-"}" +
        "|uncached=${usage.uncachedInputTokenCount ?: "-"}" +
        "|output=${usage.outputTokenCount ?: "-"}" +
        "|reasoning=${usage.reasoningTokenCount ?: "-"}"

private fun eventLine(event: StreamEvent): String = when (event) {
    is StreamEvent.TextChunk -> "text|" + esc(event.text)
    is StreamEvent.ThoughtChunk ->
        "thought|" + esc(event.thought) + "|title=${orNull(event.title)}" +
            "|signature=${orNull(event.signature)}"
    is StreamEvent.ToolCallUpdate ->
        "toolCallUpdate|streamKey=${norm(event.streamKey)}|id=${orNull(event.id?.let(::norm))}" +
            "|name=${esc(event.name)}|args=${esc(event.arguments)}" +
            "|signature=${orNull(event.signature)}"
    is StreamEvent.ToolCallRequest ->
        "toolCall|id=${norm(event.id)}|name=${esc(event.name)}|args=${esc(event.arguments)}" +
            "|streamKey=${norm(event.streamKey)}|signature=${orNull(event.signature)}"
    is StreamEvent.ToolCallsRequest ->
        "toolCalls|calls=" + event.calls.joinToString(";") { call ->
            "id=${norm(call.id)}~name=${esc(call.name)}~args=${esc(call.arguments)}" +
                "~streamKey=${norm(call.streamKey)}"
        }
    is StreamEvent.UsageUpdate -> usageLine(event.usage)
    else -> error("unexpected event in the loop: ${event::class.simpleName}")
}

private fun errorLine(error: com.newoether.agora.api.GenerationError?): String =
    when (error) {
        null -> "none"
        is com.newoether.agora.api.GenerationError.Api ->
            "api|code=${orNull(error.code)}|type=${orNull(error.type)}|message=${esc(error.message)}"
        is com.newoether.agora.api.GenerationError.SseParse ->
            "sseParse|raw=${esc(error.rawLine)}"
        else -> error("unexpected stream error: ${error::class.simpleName}")
    }

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: OllamaTraceDump <scenario.json> <filesRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val filesRoot = File(args[1]).canonicalFile
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    try {
        when (scenario.mode) {
            "serialized" -> {
                requireValidSerializedRequest(
                    provider = "Ollama",
                    body = requireNotNull(scenario.serializedBody) {
                        "serialized mode requires serializedBody"
                    },
                    requiredStringFields = scenario.serializedRequiredStringFields.toSet(),
                    requiredArrayFields = scenario.serializedRequiredArrayFields.toSet(),
                )
                line("ok")
            }
            "validate" -> {
                val requestJson = requireNotNull(scenario.request) { "validate mode requires request" }
                val request = traceJson.decodeFromJsonElement(OllamaChatRequest.serializer(), requestJson)
                request.requireValidWireFormat()
                line("ok")
            }
            "stream" -> {
                runStreamLoop(scenario, out)
            }
            else -> {
                val config = buildConfig(scenario.config)
                val input = ProviderRequestInput(
                    messages = scenario.requestInput?.messages.orEmpty()
                        .map { buildMessage(filesRoot, it) },
                    systemPrompt = scenario.requestInput?.systemPrompt,
                )
                val base64Files = Base64FileRegistry()
                val built = buildRequestBody(config, input, base64Files)
                line(
                    "apiMessages",
                    norm(
                        esc(
                            traceJson.encodeToString(
                                ListSerializer(OllamaMessage.serializer()),
                                built.apiMessages,
                            ),
                        ),
                    ),
                )
                line("body", norm(esc(built.bodyJson)))
                built.streamingBody?.let { body ->
                    line("diagnostic", norm(esc(body.diagnosticJson)))
                    line("contentLength", body.contentLength.toString())
                    body.parts.forEachIndexed { index, part ->
                        line(
                            "file",
                            index.toString(),
                            esc(relPath(filesRoot.toPath(), part.file)),
                            part.byteCount.toString(),
                            hex(part.sha256),
                        )
                    }
                    val resolved = resolvedBodyText(body)
                    val resolvedBytes = resolved.toByteArray(Charsets.UTF_8)
                    require(resolvedBytes.size.toLong() == body.contentLength) {
                        "resolved body is ${resolvedBytes.size} bytes but contentLength is ${body.contentLength}"
                    }
                    line("resolvedBody", norm(esc(resolved)))
                }
            }
        }
    } catch (error: Exception) {
        // Only the request-format gate carries a portable, semantic message.
        val message = if (error is RequestFormatException) esc(error.message ?: "") else ""
        line("error", error.javaClass.simpleName ?: "unknown", message)
    }
    print(out)
}

private fun emitLine(out: StringBuilder, vararg fields: String) {
    out.append(fields.joinToString("|")).append('\n')
}

/**
 * The 200-branch of generateResponse, transcribed: raw NDJSON lines (no data: prefix), the batched
 * tool-call snapshot handling, done/done_reason/error handling and the break conditions.
 */
private fun runStreamLoop(scenario: Scenario, out: StringBuilder) {
    var producedContent = false
    var sawDone = false
    var doneReason: String? = null
    var streamError: com.newoether.agora.api.GenerationError? = null
    var toolCallInFlight = false  // never set by the loop; kept to mirror the termination input

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
        val rawLine = op.substring(2)
        try {
            val response = traceJson.decodeFromString(OllamaStreamResponse.serializer(), rawLine)
            response.doneReason?.takeIf(String::isNotBlank)?.let { doneReason = it }
            response.error?.takeIf(String::isNotBlank)?.let { message ->
                streamError = com.newoether.agora.api.GenerationError.Api(
                    code = null,
                    type = "ollama_stream_error",
                    message = message,
                )
            }
            response.message?.let { msg ->
                // 1. Explicit thinking field (Ollama 0.5.4+)
                msg.thinking?.let { thinking ->
                    if (thinking.isNotEmpty()) {
                        emitTracked(StreamEvent.ThoughtChunk(thinking, null))
                    }
                }

                // 2. Complete snapshot batches: rejected as a whole when any call is damaged.
                msg.toolCalls?.takeIf { it.isNotEmpty() }?.let { toolCalls ->
                    val parsed = toolCalls.map { tc ->
                        val streamKey = "call_stream_${UUID.randomUUID()}"
                        val id = tc.id?.takeIf(String::isNotBlank)
                            ?: "${Constants.TOOL_CALL_ID_PREFIX}${UUID.randomUUID()}"
                        val name = tc.function?.name.orEmpty()
                        val args = tc.function?.arguments?.let {
                            if (it is JsonPrimitive && it.isString) {
                                it.content
                            } else {
                                it.toString()
                            }
                        }.orEmpty().ifBlank { "{}" }
                        Triple(
                            StreamEvent.ToolCallUpdate(
                                streamKey = streamKey,
                                id = id,
                                name = name,
                                arguments = args,
                            ),
                            StreamEvent.ToolCallRequest(
                                id = id,
                                name = name,
                                arguments = args,
                                streamKey = streamKey,
                            ),
                            runCatching {
                                name.matches(safeWireToolName) &&
                                    id.matches(safeWireToolCallId) &&
                                    traceJson.parseToJsonElement(args) is JsonObject
                            }.getOrDefault(false),
                        )
                    }
                    val seenIds = mutableSetOf<String>()
                    val calls = parsed.map { (_, call, valid) ->
                        if (valid && seenIds.add(call.id)) {
                            call
                        } else {
                            malformedToolCallRequest(
                                cause = "Ollama returned incomplete or duplicate tool metadata",
                                originalName = call.name,
                                originalArguments = call.arguments,
                                streamKey = call.streamKey,
                            )
                        }
                    }
                    parsed.forEach { emitTracked(it.first) }
                    if (calls.size == 1) emitTracked(calls.single())
                    else emitTracked(StreamEvent.ToolCallsRequest(calls))
                }

                // 3. Inline content
                if (msg.content.isNotEmpty()) {
                    emitTracked(StreamEvent.TextChunk(msg.content))
                }
            }
            if (response.done) {
                sawDone = true
                emitTracked(StreamEvent.UsageUpdate(response.toTokenUsage()))
            }
            if (streamError != null || sawDone) stopped = true
        } catch (error: Exception) {
            streamError = com.newoether.agora.api.GenerationError.SseParse(
                rawLine = rawLine.take(512),
                cause = error.localizedMessage ?: "Malformed Ollama stream payload",
            )
            stopped = true
        }
    }

    val termination = ollamaStreamTermination(
        sawDone = sawDone,
        doneReason = doneReason,
        producedContent = producedContent,
        toolCallInFlight = toolCallInFlight,
        streamError = streamError,
        timedOut = false,
    )
    emitLine(
        out,
        "term",
        "sawDone=${if (termination.sawTerminalMarker) 1 else 0}",
        "stop=${orNull(termination.stopReason)}",
        "produced=${if (termination.producedContent) 1 else 0}",
        "inFlight=${if (termination.toolCallInFlight) 1 else 0}",
    )
    emitLine(out, "streamError", errorLine(termination.streamError))
}

private fun buildConfig(config: ScenarioConfig): ProviderConfig = ProviderConfig(
    apiKey = "scenario-key",
    modelId = config.modelId,
    thinkingEnabled = config.thinkingEnabled,
    thinkingLevel = config.thinkingLevel,
    thinkingBudgetEnabled = config.thinkingBudgetEnabled,
    thinkingBudgetTokens = config.thinkingBudgetTokens,
    includeImages = config.includeImages,
    includeAssistantReasoning = false,
    googleSearchEnabled = false,
    openAiServiceTier = null,
    temperature = config.temperature?.let { scenarioFloat(it) },
    maxTokens = config.maxTokens,
    topP = config.topP?.let { scenarioFloat(it) },
    frequencyPenalty = null,
    presencePenalty = null,
    promptCacheKey = null,
    tools = buildTools(config.tools),
)