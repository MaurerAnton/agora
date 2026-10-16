// Golden trace driver for the OpenAI chat-completions request pipeline, used to pin the C++ port.
//
// Runs the REAL Kotlin pipeline (OpenAiChatRequestFactory + MessageConverter + provider thinking
// hooks + kotlinx serialization + Base64FileRegistry, compiled from the app sources) over a canned
// JSON scenario and prints a canonical trace. The C++ core must reproduce the same trace byte for
// byte for every scenario.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package openai.request.golden

import com.newoether.agora.api.OpenAiChatRequest
import com.newoether.agora.api.OpenAiMessage
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.ProviderRequestInput
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.openai.OpenAiChatRequestFactory
import com.newoether.agora.api.openai.requireValidWireFormat
import com.newoether.agora.api.openai.withCustomOpenAiThinking
import com.newoether.agora.api.openai.withDeepSeekThinking
import com.newoether.agora.api.openai.withGroqThinking
import com.newoether.agora.api.openai.withOpenAiThinking
import com.newoether.agora.api.openai.withOpenRouterReasoning
import com.newoether.agora.api.openai.withQwenThinking
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.util.StreamingJsonBody
import com.newoether.agora.api.util.resolvedThinking
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.Participant
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.model.ToolCallData
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.decodeFromJsonElement
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.file.Path
import java.util.Base64

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

private val uuidPattern = Regex(
    "[0-9a-f]{8}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{4}-[0-9a-f]{12}|[0-9a-f]{32}"
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

private fun customizerFor(
    kind: String,
    defaultBaseUrl: String,
): (OpenAiChatRequest, ProviderConfig) -> OpenAiChatRequest = when (kind) {
    "openai" -> { request, config ->
        request.withOpenAiThinking(config.resolvedThinking(ThinkingProviderFamily.OPENAI))
    }
    "deepseek" -> { request, config ->
        request.withDeepSeekThinking(config.resolvedThinking(ThinkingProviderFamily.DEEPSEEK))
    }
    "groq" -> { request, config ->
        request.withGroqThinking(
            config.resolvedThinking(ThinkingProviderFamily.GROQ),
            config.thinkingEnabled,
        )
    }
    "qwen" -> { request, config ->
        request.withQwenThinking(config.resolvedThinking(ThinkingProviderFamily.QWEN))
    }
    "openrouter" -> { request, config ->
        request.withOpenRouterReasoning(
            config.resolvedThinking(ThinkingProviderFamily.OPENROUTER),
            config.googleSearchEnabled,
        )
    }
    "custom" -> { request, config ->
        request.withCustomOpenAiThinking(
            config.resolvedThinking(ThinkingProviderFamily.OPENAI_COMPATIBLE),
            defaultBaseUrl,
        )
    }
    else -> error("unknown provider kind: $kind")
}

/** Mirrors the providers' `forwardsAssistantReasoningContent` overrides. */
private fun forwardsReasoning(kind: String, config: ProviderConfig): Boolean = when (kind) {
    "deepseek", "custom" -> config.includeAssistantReasoning
    else -> false
}

private fun resolveMedia(filesRoot: File, reference: String): File =
    File(filesRoot, reference).canonicalFile

@Serializable
private data class Scenario(
    val name: String,
    val kind: String,
    val providerName: String,
    val providerBaseUrl: String? = null,
    val mode: String = "factory",
    val config: ScenarioConfig = ScenarioConfig(),
    val requestInput: ScenarioInput? = null,
    val request: JsonObject? = null,
)

@Serializable
private data class ScenarioConfig(
    val modelId: String = "gpt-4.1",
    val thinkingEnabled: Boolean = true,
    val thinkingLevel: String = "medium",
    val thinkingBudgetEnabled: Boolean = false,
    val thinkingBudgetTokens: Int = 4096,
    val includeImages: Boolean = true,
    val includeAssistantReasoning: Boolean = false,
    val googleSearchEnabled: Boolean = false,
    val openAiServiceTier: String? = null,
    val temperature: JsonPrimitive? = null,
    val maxTokens: Int? = null,
    val topP: JsonPrimitive? = null,
    val frequencyPenalty: JsonPrimitive? = null,
    val presencePenalty: JsonPrimitive? = null,
    val promptCacheKey: String? = null,
    val tools: List<ToolDefinition>? = null,
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

private fun buildConfig(config: ScenarioConfig): ProviderConfig = ProviderConfig(
    apiKey = "scenario-key",
    modelId = config.modelId,
    thinkingEnabled = config.thinkingEnabled,
    thinkingLevel = config.thinkingLevel,
    thinkingBudgetEnabled = config.thinkingBudgetEnabled,
    thinkingBudgetTokens = config.thinkingBudgetTokens,
    includeImages = config.includeImages,
    includeAssistantReasoning = config.includeAssistantReasoning,
    googleSearchEnabled = config.googleSearchEnabled,
    openAiServiceTier = config.openAiServiceTier,
    temperature = config.temperature?.let { it.content.toFloat() },
    maxTokens = config.maxTokens,
    topP = config.topP?.let { it.content.toFloat() },
    frequencyPenalty = config.frequencyPenalty?.let { it.content.toFloat() },
    presencePenalty = config.presencePenalty?.let { it.content.toFloat() },
    promptCacheKey = config.promptCacheKey,
    tools = config.tools,
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: RequestGoldenTraceDump <scenario.json> <filesRoot>" }
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
    line("kind", esc(scenario.kind))
    line("mode", esc(scenario.mode))
    try {
        if (scenario.mode == "validate") {
            val requestJson = requireNotNull(scenario.request) { "validate mode requires request" }
            val request = Json { ignoreUnknownKeys = true }
                .decodeFromJsonElement(OpenAiChatRequest.serializer(), requestJson)
            request.requireValidWireFormat(scenario.providerName)
            line("ok")
        } else {
            val config = buildConfig(scenario.config)
            val input = ProviderRequestInput(
                messages = scenario.requestInput?.messages.orEmpty()
                    .map { buildMessage(filesRoot, it) },
                systemPrompt = scenario.requestInput?.systemPrompt,
            )
            val factory = OpenAiChatRequestFactory(
                providerName = scenario.providerName,
                json = traceJson,
                customizeRequest = customizerFor(
                    scenario.kind,
                    scenario.providerBaseUrl ?: "https://relay.example/v1",
                ),
                transformSystemPrompt = { it },
                forwardsAssistantReasoning = { forwardsReasoning(scenario.kind, it) },
            )
            val built = factory.build(config, input)
            line(
                "apiMessages",
                norm(
                    esc(
                        traceJson.encodeToString(
                            ListSerializer(OpenAiMessage.serializer()),
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
                line("resolvedBody", norm(esc(resolvedBodyText(body))))
            }
        }
    } catch (error: Exception) {
        // Only the request-format gate carries a portable, semantic message; other exception texts
        // belong to the Kotlin runtime and are not part of the contract.
        val message = if (error is RequestFormatException) esc(error.message ?: "") else ""
        line("error", error.javaClass.simpleName ?: "unknown", message)
    }
    print(out)
}

private fun relPath(filesRoot: Path, file: File): String =
    filesRoot.relativize(file.toPath()).toString()
