// Golden trace driver for the Anthropic request pipeline, used to pin the C++ port.
//
// Runs the REAL Kotlin pipeline logic (message conversion, thinking resolution, kotlinx
// serialization + Base64FileRegistry, compiled from the app sources) over a canned JSON scenario
// and prints a canonical trace. The C++ core must reproduce the same trace byte for byte for every
// scenario.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package anthropic.request.golden

import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.ProviderRequestInput
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty
import com.newoether.agora.api.anthropic.AnthropicRequest
import com.newoether.agora.api.anthropic.AnthropicMessage
import com.newoether.agora.api.anthropic.AnthropicContentPart
import com.newoether.agora.api.anthropic.AnthropicThinking
import com.newoether.agora.api.anthropic.AnthropicOutputConfig
import com.newoether.agora.api.anthropic.AnthropicCacheControl
import com.newoether.agora.api.anthropic.AnthropicTool
import com.newoether.agora.api.anthropic.AnthropicImageSource
import com.newoether.agora.api.anthropic.requireValidWireFormat
import com.newoether.agora.api.anthropic.signatureIsCompatibleWithAnthropic
import com.newoether.agora.api.anthropic.isAnthropicToolRoundCompatible
import com.newoether.agora.api.util.Base64FileRegistry
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.util.StreamingJsonBody
import com.newoether.agora.api.util.ToolSchemaJson
import com.newoether.agora.api.util.adaptToolRoundsForProvider
import com.newoether.agora.api.util.requireValidSerializedRequest
import com.newoether.agora.api.util.resolvedThinking
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

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

// ---- Replicated from AnthropicProvider.kt (private/internal members not visible across modules) ----

/** From AnthropicProvider.Companion */
const val ANSWER_HEADROOM_TOKENS = 8192
const val DEFAULT_MAX_TOKENS = 32768

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
)

/** Random UUIDs (base64 placeholders, scenario data) are not comparable across implementations. */
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
    val providerName: String = "Anthropic",
    val mode: String = "factory",
    val config: ScenarioConfig = ScenarioConfig(),
    val requestInput: ScenarioInput? = null,
    val request: JsonObject? = null,
    val serializedBody: String? = null,
    val serializedRequiredStringFields: List<String> = emptyList(),
    val serializedRequiredArrayFields: List<String> = emptyList(),
)

@Serializable
private data class ScenarioConfig(
    val modelId: String = "claude-3-5-sonnet",
    val thinkingEnabled: Boolean = true,
    val thinkingLevel: String = "medium",
    val thinkingBudgetEnabled: Boolean = false,
    val thinkingBudgetTokens: Int = 4096,
    val includeImages: Boolean = true,
    val anthropicCacheEnabled: Boolean = true,
    val anthropicCacheTtl: String = "1h",
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
    val responseOutputItems: List<JsonObject> = emptyList(),
    val responseOutputItemProvider: String? = null,
)

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
            responseOutputItems = call.responseOutputItems,
            responseOutputItemProvider = call.responseOutputItemProvider,
        )
    },
)

private fun resolveMedia(filesRoot: File, reference: String): File =
    File(filesRoot, reference).canonicalFile

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

/**
 * Replicates AnthropicProvider.buildRequestBody logic using only internal/public APIs.
 * This mirrors the exact logic from the provider's private function, but uses only
 * the internal utilities that are visible across modules.
 */
private fun buildRequestBody(
    config: ProviderConfig,
    input: ProviderRequestInput,
    filesRoot: File,
): BuiltAnthropicRequest {
    val base64Files = Base64FileRegistry()
    val modelName = config.modelId

    if (config.anthropicCacheEnabled && config.anthropicCacheTtl !in setOf("5m", "1h")) {
        throw RequestFormatException("Anthropic", listOf("Invalid Anthropic cache duration"))
    }

    val resolvedThinking = config.resolvedThinking(ThinkingProviderFamily.ANTHROPIC)
    val capability = resolvedThinking.capability
    val thinkingBudget = resolvedThinking.budgetTokens
    val hasThinkingControls = capability.supportsEffort || capability.supportsThinkingBudget
    val thinking = when {
        !hasThinkingControls -> null
        resolvedThinking.disabled -> com.newoether.agora.api.anthropic.AnthropicThinking(type = "disabled")
        thinkingBudget != null ->
            com.newoether.agora.api.anthropic.AnthropicThinking(type = "enabled", budgetTokens = thinkingBudget, display = "summarized")
        capability.supportsEffort -> com.newoether.agora.api.anthropic.AnthropicThinking(type = "adaptive", display = "summarized")
        else -> com.newoether.agora.api.anthropic.AnthropicThinking(
            type = "enabled",
            budgetTokens = capability.clampBudget(config.thinkingBudgetTokens),
            display = "summarized",
        )
    }
    val outputConfig = (resolvedThinking.effort ?: capability.nearestEffort(config.thinkingLevel))
        ?.let { com.newoether.agora.api.anthropic.AnthropicOutputConfig(effort = it) }

    val anthropicTools = config.tools?.map { td ->
        com.newoether.agora.api.anthropic.AnthropicTool(
            name = td.function.name,
            description = td.function.description,
            inputSchema = ToolSchemaJson.of(td.function.parameters)
        )
    }

    val validatedPath = adaptToolRoundsForProvider(
        messages = input.messages,
        providerName = "Anthropic",
    ) { toolMessage ->
        toolMessage.isAnthropicToolRoundCompatible(
            targetModel = modelName,
            targetProviderName = "Anthropic",
            signedThinkingRequired = thinking?.type in setOf("enabled", "adaptive"),
        )
    }

    val apiMessages = com.newoether.agora.api.anthropic.coalesceAnthropicMessages(buildList {
        var index = 0
        while (index < validatedPath.size) {
            val message = validatedPath[index]
            when {
                message.id.startsWith(Constants.TOOL_MSG_PREFIX) -> {
                    add(buildAssistantToolUse(message, modelName, "Anthropic", thinking, base64Files, config.includeImages))
                    index++
                    if (
                        index < validatedPath.size &&
                        validatedPath[index].id.startsWith(Constants.RESULT_MSG_PREFIX)
                    ) {
                        val resultBlocks = mutableListOf<com.newoether.agora.api.anthropic.AnthropicContentPart>()
                        while (
                            index < validatedPath.size &&
                            validatedPath[index].id.startsWith(Constants.RESULT_MSG_PREFIX)
                        ) {
                            resultBlocks.addAll(buildToolResultBlocks(validatedPath[index]))
                            index++
                        }
                        add(com.newoether.agora.api.anthropic.AnthropicMessage(role = "user", content = resultBlocks))
                    }
                }
                message.id.startsWith(Constants.RESULT_MSG_PREFIX) -> index++
                else -> {
                    add(buildNormalMessage(
                        if (config.includeImages) message
                        else message.copy(images = emptyList()),
                        base64Files,
                    ))
                    index++
                }
            }
        }
    })

    val maxTokens = config.maxTokens ?: run {
        val wanted = when {
            thinking?.budgetTokens != null ->
                maxOf(thinking.budgetTokens + ANSWER_HEADROOM_TOKENS, DEFAULT_MAX_TOKENS)
            else -> DEFAULT_MAX_TOKENS
        }
        com.newoether.agora.api.anthropic.AnthropicOutputLimits.maxOutputTokens(modelName)?.let { minOf(wanted, it) } ?: wanted
    }

    val request = com.newoether.agora.api.anthropic.AnthropicRequest(
        model = modelName,
        messages = apiMessages,
        system = input.systemPrompt,
        cacheControl = if (config.anthropicCacheEnabled) {
            com.newoether.agora.api.anthropic.AnthropicCacheControl(ttl = config.anthropicCacheTtl)
        } else null,
        thinking = thinking,
        outputConfig = outputConfig,
        maxTokens = maxTokens,
        tools = anthropicTools,
        temperature = config.temperature.takeIf {
            capability.supportsSamplingParams && thinking == null
        },
        topP = config.topP?.takeIf {
            capability.supportsSamplingParams && (thinking == null || it in 0.95f..1f)
        }
    )

    request.requireValidWireFormat()

    val requestBodyJson = traceJson.encodeToString(com.newoether.agora.api.anthropic.AnthropicRequest.serializer(), request)
    val streamingRequest = base64Files.prepare(requestBodyJson)
    com.newoether.agora.api.util.requireValidSerializedRequest(
        provider = "Anthropic",
        body = requestBodyJson,
        requiredStringFields = setOf("model"),
        requiredArrayFields = setOf("messages"),
    )

    return BuiltAnthropicRequest(apiMessages, requestBodyJson, streamingRequest)
}

/** Replicates AnthropicProvider.buildAssistantToolUse using only public/internal APIs. */
private fun buildAssistantToolUse(
    msg: ChatMessage,
    targetModel: String,
    providerName: String,
    thinking: com.newoether.agora.api.anthropic.AnthropicThinking?,
    base64Files: Base64FileRegistry,
    includeImages: Boolean,
): com.newoether.agora.api.anthropic.AnthropicMessage {
    val thinkingEnabled = thinking?.type in setOf("enabled", "adaptive")
    val thinkingParts = msg.segments
        ?.filter {
            it.type == "thought" &&
            it.content.isNotEmpty() &&
            !it.signature.isNullOrBlank() &&
            it.signatureIsCompatibleWithAnthropic(msg.modelName, targetModel, providerName)
        }
        ?.map { com.newoether.agora.api.anthropic.AnthropicContentPart(type = "thinking", thinking = it.content, signature = it.signature) }
        .orEmpty()
    val toolSegs = msg.segments?.filter { it.type == "tool" }
    if (!toolSegs.isNullOrEmpty()) {
        val blocks = toolSegs.map { seg -> buildToolUseBlock(seg.toolCallId, seg.toolName, seg.toolArgs) }
        return com.newoether.agora.api.anthropic.AnthropicMessage(role = "assistant", content = thinkingParts + blocks)
    }
    val tc = msg.toolCall ?: return com.newoether.agora.api.anthropic.AnthropicMessage(role = "assistant", content = listOf(
        com.newoether.agora.api.anthropic.AnthropicContentPart(type = "text", text = "Continue")
    ))
    val block = buildToolUseBlock(tc.toolCallId, tc.toolName, tc.arguments)
    return com.newoether.agora.api.anthropic.AnthropicMessage(role = "assistant", content = thinkingParts + listOf(block))
}

/** Replicates AnthropicProvider.buildToolUseBlock. */
private fun buildToolUseBlock(id: String?, name: String?, args: String?): com.newoether.agora.api.anthropic.AnthropicContentPart {
    val toolId = id ?: com.newoether.agora.api.util.buildToolCallId(name ?: "", args ?: "{}", "tool_")
    val input = try {
        traceJson.parseToJsonElement(args ?: "{}") as? JsonObject ?: JsonObject(emptyMap())
    } catch (_: Exception) { JsonObject(emptyMap()) }
    return com.newoether.agora.api.anthropic.AnthropicContentPart(type = "tool_use", id = toolId, name = name ?: "", input = input)
}

/** Replicates AnthropicProvider.buildToolResultBlocks. */
private fun buildToolResultBlocks(msg: ChatMessage): List<com.newoether.agora.api.anthropic.AnthropicContentPart> {
    val toolSegs = msg.segments?.filter { it.type == "tool" }
    if (!toolSegs.isNullOrEmpty()) {
        return toolSegs.map { seg ->
            val toolId = seg.toolCallId ?: com.newoether.agora.api.util.buildToolCallId(seg.toolName ?: "", seg.toolArgs ?: "{}", "tool_")
            com.newoether.agora.api.anthropic.AnthropicContentPart(type = "tool_result", toolUseId = toolId, content = seg.toolResult ?: "")
        }
    }
    val tc = msg.toolCall ?: return emptyList()
    val toolId = tc.toolCallId ?: com.newoether.agora.api.util.buildToolCallId(tc.toolName, tc.arguments, "tool_")
    return listOf(com.newoether.agora.api.anthropic.AnthropicContentPart(type = "tool_result", toolUseId = toolId, content = tc.result))
}

/** Replicates AnthropicProvider.buildNormalMessage. */
private fun buildNormalMessage(
    msg: ChatMessage,
    base64Files: Base64FileRegistry,
): com.newoether.agora.api.anthropic.AnthropicMessage {
    val parts = mutableListOf<com.newoether.agora.api.anthropic.AnthropicContentPart>()
    val imagePaths = if (msg.participant == Participant.USER) msg.images else emptyList()
    for (imagePath in imagePaths) {
        val placeholder = base64Files.register(imagePath) ?: continue
        parts.add(
            com.newoether.agora.api.anthropic.AnthropicContentPart(
                type = "image",
                source = com.newoether.agora.api.anthropic.AnthropicImageSource(
                    mediaType = com.newoether.agora.api.util.imageMimeType(imagePath),
                    data = placeholder,
                ),
            ),
        )
    }
    // isNotBlank, NOT isNotEmpty
    if (msg.text.isNotBlank()) {
        parts.add(com.newoether.agora.api.anthropic.AnthropicContentPart(type = "text", text = msg.text))
    }
    if (parts.isEmpty()) parts.add(com.newoether.agora.api.anthropic.AnthropicContentPart(type = "text", text = "Continue"))
    val role = if (msg.participant == Participant.USER) "user" else "assistant"
    return com.newoether.agora.api.anthropic.AnthropicMessage(role = role, content = parts)
}

internal data class BuiltAnthropicRequest(
    val apiMessages: List<com.newoether.agora.api.anthropic.AnthropicMessage>,
    val bodyJson: String,
    val streamingBody: StreamingJsonBody?
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: AnthropicRequestTraceDump <scenario.json> <filesRoot>" }
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
        if (scenario.mode == "serialized") {
            requireValidSerializedRequest(
                provider = scenario.providerName,
                body = requireNotNull(scenario.serializedBody) {
                    "serialized mode requires serializedBody"
                },
                requiredStringFields = scenario.serializedRequiredStringFields.toSet(),
                requiredArrayFields = scenario.serializedRequiredArrayFields.toSet(),
            )
            line("ok")
        } else if (scenario.mode == "validate") {
            val requestJson = requireNotNull(scenario.request) { "validate mode requires request" }
            val request = traceJson.decodeFromJsonElement(com.newoether.agora.api.anthropic.AnthropicRequest.serializer(), requestJson)
            request.requireValidWireFormat()
            line("ok")
        } else {
            val config = buildConfig(scenario.config)
            val input = ProviderRequestInput(
                messages = scenario.requestInput?.messages.orEmpty()
                    .map { buildMessage(filesRoot, it) },
                systemPrompt = scenario.requestInput?.systemPrompt,
            )
            val built = buildRequestBody(config, input, filesRoot)
            line(
                "apiMessages",
                norm(
                    esc(
                        traceJson.encodeToString(
                            ListSerializer(com.newoether.agora.api.anthropic.AnthropicMessage.serializer()),
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
    } catch (error: Exception) {
        // Only the request-format gate carries a portable, semantic message.
        val message = if (error is RequestFormatException) esc(error.message ?: "") else ""
        line("error", error.javaClass.simpleName ?: "unknown", message)
    }
    print(out)
}

private fun buildConfig(config: ScenarioConfig): ProviderConfig = ProviderConfig(
    apiKey = "scenario-key",
    modelId = config.modelId,
    thinkingEnabled = config.thinkingEnabled,
    thinkingLevel = config.thinkingLevel,
    thinkingBudgetEnabled = config.thinkingBudgetEnabled,
    thinkingBudgetTokens = config.thinkingBudgetTokens,
    anthropicCacheEnabled = config.anthropicCacheEnabled,
    anthropicCacheTtl = config.anthropicCacheTtl,
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