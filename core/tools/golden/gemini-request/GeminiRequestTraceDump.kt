// Golden trace driver for the Gemini request pipeline, used to pin the C++ port.
//
// Runs the REAL Kotlin declarations (wire models + validator compiled from the app sources, the
// request-side helpers extracted verbatim from GeminiProvider.kt) over a canned JSON scenario and
// prints a canonical trace. The C++ core must reproduce the same trace byte for byte.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package gemini.request.golden

import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.ProviderRequestInput
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty
import com.newoether.agora.api.gemini.ApiExecutableCode
import com.newoether.agora.api.gemini.ApiGenerationConfig
import com.newoether.agora.api.gemini.ApiGenerateContentRequest
import com.newoether.agora.api.gemini.ApiInlineData
import com.newoether.agora.api.gemini.ApiRequestContent
import com.newoether.agora.api.gemini.ApiRequestPart
import com.newoether.agora.api.gemini.ApiTool
import com.newoether.agora.api.gemini.ApiToolConfig
import com.newoether.agora.api.gemini.ApiThinkingConfig
import com.newoether.agora.api.gemini.GeminiFunctionCall
import com.newoether.agora.api.gemini.GeminiFunctionDeclaration
import com.newoether.agora.api.gemini.GeminiFunctionResponse
import com.newoether.agora.api.gemini.coalesceGeminiContents
import com.newoether.agora.api.gemini.geminiModelMessageParts
import com.newoether.agora.api.gemini.isGeminiToolRoundCompatible
import com.newoether.agora.api.gemini.requireValidWireFormat
import com.newoether.agora.api.gemini.stringContent
import com.newoether.agora.api.util.Base64FileRegistry
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.util.StreamingJsonBody
import com.newoether.agora.api.util.ToolSchemaJson
import com.newoether.agora.api.util.adaptToolRoundsForProvider
import com.newoether.agora.api.util.imageMimeType
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

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}",
)

/** Random UUIDs (base64 placeholders) are not comparable across implementations. */
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
    val providerName: String = "Google",
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
    val modelId: String = "gemini-2.5-pro",
    val thinkingEnabled: Boolean = true,
    val thinkingLevel: String = "medium",
    val thinkingBudgetEnabled: Boolean = false,
    val thinkingBudgetTokens: Int = 4096,
    val codeExecutionEnabled: Boolean = false,
    val googleSearchEnabled: Boolean = false,
    val includeImages: Boolean = true,
    val temperature: JsonPrimitive? = null,
    val maxTokens: Int? = null,
    val topP: JsonPrimitive? = null,
    val frequencyPenalty: JsonPrimitive? = null,
    val presencePenalty: JsonPrimitive? = null,
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

// ---- Transcribed from GeminiProvider.kt (private/local members not compilable standalone) --------

/** GeminiProvider.buildGeminiFunctionResponse. */
private fun buildGeminiFunctionResponse(result: String): JsonObject {
    return try {
        val parsed = traceJson.parseToJsonElement(result)
        when (parsed) {
            is JsonObject -> parsed
            else -> JsonObject(mapOf("result" to parsed))
        }
    } catch (_: Exception) {
        JsonObject(mapOf("result" to JsonPrimitive(result)))
    }
}

/** GeminiProvider.buildApiContents (a local function inside generateResponse). */
private fun buildApiContents(
    resolvedMessages: List<ChatMessage>,
    base64Files: Base64FileRegistry,
    config: ProviderConfig,
    providerName: String,
    cleanModelName: String,
    requiresFunctionCallSignature: Boolean,
): List<ApiRequestContent> {
    val validatedPath = adaptToolRoundsForProvider(
        messages = resolvedMessages,
        providerName = providerName,
    ) { toolMessage ->
        toolMessage.isGeminiToolRoundCompatible(
            targetModel = cleanModelName,
            targetProviderName = providerName,
            signatureRequired = requiresFunctionCallSignature,
        )
    }

    return coalesceGeminiContents(validatedPath.flatMap { msg ->
        val entries = mutableListOf<ApiRequestContent>()

        // tool_ messages: model turn with functionCall(s)
        if (msg.id.startsWith(Constants.TOOL_MSG_PREFIX)) {
            val toolSegs = msg.segments?.filter { it.type == "tool" }
            if (!toolSegs.isNullOrEmpty()) {
                val parts = toolSegs.map { seg ->
                    val args = try {
                        traceJson.parseToJsonElement(seg.toolArgs ?: "{}") as? JsonObject
                    } catch (_: Exception) { JsonObject(emptyMap()) }
                    ApiRequestPart(
                        functionCall = GeminiFunctionCall(
                            id = seg.toolCallId,
                            name = seg.toolName ?: "",
                            args = args ?: JsonObject(emptyMap())
                        ),
                        thoughtSignature = seg.signature
                    )
                }
                entries.add(ApiRequestContent(role = "model", parts = parts))
            } else if (msg.toolCall != null) {
                val args = try {
                    traceJson.parseToJsonElement(msg.toolCall!!.arguments) as? JsonObject
                } catch (_: Exception) { JsonObject(emptyMap()) }
                entries.add(ApiRequestContent(
                    role = "model",
                    parts = listOf(ApiRequestPart(
                        functionCall = GeminiFunctionCall(
                            id = msg.toolCall!!.toolCallId,
                            name = msg.toolCall!!.toolName,
                            args = args ?: JsonObject(emptyMap())
                        ),
                        thoughtSignature = msg.toolCall!!.signature
                    ))
                ))
            }
            return@flatMap entries
        }

        // result_ messages carry the function response(s)
        if (msg.id.startsWith(Constants.RESULT_MSG_PREFIX)) {
            val toolSegs = msg.segments?.filter { it.type == "tool" }
            if (!toolSegs.isNullOrEmpty()) {
                val parts = toolSegs.map { seg ->
                    val response = buildGeminiFunctionResponse(seg.toolResult ?: "{}")
                    ApiRequestPart(functionResponse = GeminiFunctionResponse(
                        id = seg.toolCallId,
                        name = seg.toolName ?: "",
                        response = response
                    ))
                }
                entries.add(ApiRequestContent(role = "user", parts = parts))
            } else if (msg.toolCall != null) {
                val response = buildGeminiFunctionResponse(msg.toolCall!!.result)
                entries.add(ApiRequestContent(
                    role = "user",
                    parts = listOf(ApiRequestPart(functionResponse = GeminiFunctionResponse(
                        id = msg.toolCall!!.toolCallId,
                        name = msg.toolCall!!.toolName,
                        response = response
                    )))
                ))
            }
            return@flatMap entries
        }

        // Normal message: preserve Gemini code-execution parts for model history.
        val parts = if (msg.participant == Participant.MODEL) {
            geminiModelMessageParts(msg).toMutableList()
        } else {
            mutableListOf<ApiRequestPart>().apply {
                if (msg.text.isNotEmpty()) add(ApiRequestPart(text = msg.text))
            }
        }
        if (config.includeImages && msg.participant == Participant.USER) {
            for (imagePath in msg.images) {
                val placeholder = base64Files.register(imagePath) ?: continue
                parts.add(
                    ApiRequestPart(
                        inlineData = ApiInlineData(
                            mimeType = imageMimeType(imagePath),
                            data = placeholder,
                        ),
                    ),
                )
            }
        }
        if (parts.isEmpty()) parts.add(ApiRequestPart(text = "[Attachment unavailable]"))
        entries.add(ApiRequestContent(
            role = if (msg.participant == Participant.USER) "user" else "model",
            parts = parts
        ))

        entries
    })
}

internal data class BuiltGeminiRequest(
    val contents: List<ApiRequestContent>,
    val bodyJson: String,
    val streamingBody: StreamingJsonBody?
)

/** The provider flow around the two local functions, in the provider's order. */
private fun buildRequestBody(
    scenario: Scenario,
    config: ProviderConfig,
    input: ProviderRequestInput,
    base64Files: Base64FileRegistry,
): BuiltGeminiRequest {
    val providerName = scenario.providerName
    val cleanModelName = config.modelId.removePrefix("models/")
    val requiresFunctionCallSignature =
        cleanModelName.contains("gemini-3", ignoreCase = true) ||
            cleanModelName.contains("gemini-3.5", ignoreCase = true)

    val tools = mutableListOf<ApiTool>()
    if (config.codeExecutionEnabled) tools.add(ApiTool(codeExecution = JsonObject(emptyMap())))
    if (config.googleSearchEnabled) tools.add(ApiTool(googleSearch = JsonObject(emptyMap())))

    val functionDeclarations = config.tools?.map { td ->
        val externalSchema = td.function.parameters.schema
        GeminiFunctionDeclaration(
            name = td.function.name,
            description = td.function.description,
            parameters = if (externalSchema == null) ToolSchemaJson.of(td.function.parameters) else null,
            parametersJsonSchema = externalSchema,
        )
    }
    if (!functionDeclarations.isNullOrEmpty()) {
        tools.add(ApiTool(functionDeclarations = functionDeclarations))
    }

    val resolvedThinking = config.resolvedThinking(ThinkingProviderFamily.GEMINI)
    val thinkingConfig = when {
        resolvedThinking.disabled -> ApiThinkingConfig(
            includeThoughts = false,
            thinkingBudget = 0.takeIf { resolvedThinking.capability.supportsThinkingBudget },
        )
        resolvedThinking.budgetTokens != null -> ApiThinkingConfig(
            includeThoughts = true,
            thinkingBudget = resolvedThinking.budgetTokens,
        )
        resolvedThinking.effort != null -> ApiThinkingConfig(
            includeThoughts = true,
            thinkingLevel = resolvedThinking.effort.uppercase(),
        )
        else -> ApiThinkingConfig(includeThoughts = true)
    }

    val hasBuiltInTools = tools.any { it.codeExecution != null || it.googleSearch != null }
    val hasFunctionDeclarations = tools.any { it.functionDeclarations != null }
    val toolConfig = if (hasBuiltInTools && hasFunctionDeclarations) {
        ApiToolConfig(includeServerSideToolInvocations = true)
    } else null

    val genConfig = ApiGenerationConfig(
        thinkingConfig = thinkingConfig,
        temperature = config.temperature,
        maxOutputTokens = config.maxTokens,
        topP = config.topP,
        frequencyPenalty = config.frequencyPenalty,
        presencePenalty = config.presencePenalty
    )

    val requestBody = ApiGenerateContentRequest(
        contents = buildApiContents(
            input.messages, base64Files, config, providerName,
            cleanModelName, requiresFunctionCallSignature,
        ),
        systemInstruction = input.systemPrompt
            ?.takeIf(String::isNotBlank)
            ?.let { ApiRequestContent(parts = listOf(ApiRequestPart(text = it))) },
        tools = if (tools.isNotEmpty()) tools else null,
        toolConfig = toolConfig,
        generationConfig = genConfig,
    )

    requestBody.requireValidWireFormat(cleanModelName)
    val requestBodyJson = traceJson.encodeToString(
        ApiGenerateContentRequest.serializer(), requestBody,
    )
    val streamingRequest = base64Files.prepare(requestBodyJson)
    requireValidSerializedRequest(
        provider = providerName,
        body = requestBodyJson,
        requiredArrayFields = setOf("contents"),
    )
    return BuiltGeminiRequest(requestBody.contents, requestBodyJson, streamingRequest)
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: GeminiRequestTraceDump <scenario.json> <filesRoot>" }
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
            val request = traceJson.decodeFromJsonElement(
                ApiGenerateContentRequest.serializer(), requestJson,
            )
            request.requireValidWireFormat(scenario.config.modelId.removePrefix("models/"))
            line("ok")
        } else {
            val config = buildConfig(scenario.config)
            val input = ProviderRequestInput(
                messages = scenario.requestInput?.messages.orEmpty()
                    .map { buildMessage(filesRoot, it) },
                systemPrompt = scenario.requestInput?.systemPrompt,
            )
            val base64Files = Base64FileRegistry()
            val built = buildRequestBody(scenario, config, input, base64Files)
            line(
                "contents",
                norm(
                    esc(
                        traceJson.encodeToString(
                            ListSerializer(ApiRequestContent.serializer()),
                            built.contents,
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
    codeExecutionEnabled = config.codeExecutionEnabled,
    googleSearchEnabled = config.googleSearchEnabled,
    includeImages = config.includeImages,
    includeAssistantReasoning = false,
    temperature = config.temperature?.let { scenarioFloat(it) },
    maxTokens = config.maxTokens,
    topP = config.topP?.let { scenarioFloat(it) },
    frequencyPenalty = config.frequencyPenalty?.let { scenarioFloat(it) },
    presencePenalty = config.presencePenalty?.let { scenarioFloat(it) },
    tools = buildTools(config.tools),
)
