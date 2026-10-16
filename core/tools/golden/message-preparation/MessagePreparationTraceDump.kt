// Golden trace driver for the provider-neutral message-preparation pipeline, used to pin the
// C++ port.
//
// Runs the REAL Kotlin sources (ApiMessagePreparation, ContextWindow, ContextLimiter,
// ToolMessages and the cost-model estimator, compiled straight from the app tree) over a canned
// JSON scenario and prints a canonical trace. The C++ core must reproduce the same trace byte
// for byte for every scenario.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package message.preparation.golden

import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.ToolFunction
import com.newoether.agora.api.ToolParameters
import com.newoether.agora.api.ToolProperty
import com.newoether.agora.api.util.canonicalContextMessages
import com.newoether.agora.api.util.contextWindowRetainedMessageIds
import com.newoether.agora.api.util.contextWindowUsage
import com.newoether.agora.api.util.limitContext
import com.newoether.agora.api.util.mergeConsecutiveSameRole
import com.newoether.agora.api.util.prepareMessages
import com.newoether.agora.api.util.projectAssistantImagesToLatestUserMessage
import com.newoether.agora.api.util.projectGenerationStatusesForApi
import com.newoether.agora.api.util.projectToolResultImagesToUserMessage
import com.newoether.agora.api.util.protocolAtomicUnits
import com.newoether.agora.api.util.splitContextForCompactRetention
import com.newoether.agora.api.util.splitLogicalContext
import com.newoether.agora.api.util.stripEmptyTurns
import com.newoether.agora.api.util.validateToolMessages
import com.newoether.agora.api.util.applyNearestContextCompact
import com.newoether.agora.api.util.tokens.ContextCostModel
import com.newoether.agora.api.util.tokens.ContextCostModels
import com.newoether.agora.api.util.tokens.CostModelContextEstimator
import com.newoether.agora.api.util.tokens.FixedContextComposition
import com.newoether.agora.api.util.tokens.bpe.O200kBase
import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.MessageSegment
import com.newoether.agora.model.MessageStatus
import com.newoether.agora.model.Participant
import com.newoether.agora.model.ToolCallData
import kotlinx.serialization.Serializable
import kotlinx.serialization.builtins.ListSerializer
import kotlinx.serialization.builtins.MapSerializer
import kotlinx.serialization.builtins.serializer
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonObject
import java.io.File

private val traceJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

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

// -- scenario shape -------------------------------------------------------------------------------

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "prepare",
    val budget: Int = 1000,
    val includeAssistantReasoning: Boolean = false,
    val fixedTokenCost: Int = 0,
    /** "Provider:model" string priced through ContextCostModels; null uses the default model. */
    val costModel: String? = null,
    /** estimateFixed modes. */
    val systemPrompt: String? = null,
    val initialUserPrompt: String? = null,
    val codeExecutionEnabled: Boolean = false,
    val googleSearchEnabled: Boolean = false,
    val openAiWebSearchEnabled: Boolean = false,
    val tools: List<JsonObject>? = null,
    /** usage mode: the fixed composition the UI shows. */
    val fixedSystemPromptTokens: Int = 0,
    val fixedToolTokens: Int = 0,
    /** split modes. */
    val retainMessages: Int = 0,
    val retainLogicalMessages: Int = 0,
    /** projection modes. */
    val includeImages: Boolean = true,
    /** estimateText mode. */
    val text: String = "",
    val messages: List<ScenarioMessage> = emptyList(),
)

@Serializable
private data class ScenarioMessage(
    val id: String,
    val parentId: String? = null,
    val participant: String,
    val status: String = "SUCCESS",
    val text: String = "",
    val images: List<String> = emptyList(),
    val thoughts: String? = null,
    val thoughtTitle: String? = null,
    val tokenCount: Int = 0,
    val modelName: String? = null,
    val timestamp: Long = 0,
    val runId: String? = null,
    val runSequence: Long? = null,
    val segments: List<ScenarioSegment>? = null,
    val toolCall: ScenarioToolCall? = null,
    val attachmentMeta: ScenarioAttachmentMeta? = null,
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
    val durationMs: Long? = null,
    val toolState: String? = null,
    val toolProgress: String? = null,
    val toolTarget: String? = null,
    val toolDisplayName: String? = null,
    val toolResultText: String? = null,
    val toolStructuredResult: String? = null,
    val toolTranscription: String? = null,
    val responseOutputItems: List<JsonObject> = emptyList(),
    val responseOutputItemProvider: String? = null,
    val errorCode: String? = null,
)

@Serializable
private data class ScenarioToolCall(
    val toolName: String,
    val arguments: String,
    val result: String,
    val signature: String? = null,
    val toolCallId: String? = null,
    val displayName: String? = null,
    val resultText: String? = null,
    val structuredResult: String? = null,
    val responseOutputItems: List<JsonObject> = emptyList(),
    val responseOutputItemProvider: String? = null,
    val transcription: String? = null,
)

@Serializable
private data class ScenarioAttachmentMeta(
    val items: List<ScenarioAttachmentItem> = emptyList(),
)

@Serializable
private data class ScenarioAttachmentItem(
    val type: String,
    val imageIndex: Int? = null,
    val pageCount: Int? = null,
    val fileSize: Long? = null,
    val pixelWidth: Int? = null,
    val pixelHeight: Int? = null,
)

// -- trace projection of a message -----------------------------------------------------------------
// Every field the preparation pipeline or the estimator reads or writes. The dump is the trace
// contract: the C++ port emits the same JSON shapes in the same order.

@Serializable
private data class SegmentDump(
    val type: String,
    val content: String = "",
    val toolName: String? = null,
    val toolArgs: String? = null,
    val toolResult: String? = null,
    val toolCallId: String? = null,
    val signature: String? = null,
    val signatureProvider: String? = null,
    val toolTranscription: String? = null,
    val errorCode: String? = null,
    val responseOutputItems: Int = 0,
    val responseOutputItemProvider: String? = null,
)

@Serializable
private data class ToolCallDump(
    val toolName: String,
    val arguments: String,
    val result: String,
    val signature: String? = null,
    val toolCallId: String? = null,
    val displayName: String? = null,
    val resultText: String? = null,
    val structuredResult: String? = null,
    val transcription: String? = null,
    val responseOutputItems: Int = 0,
    val responseOutputItemProvider: String? = null,
)

@Serializable
private data class AttachmentItemDump(
    val type: String,
    val imageIndex: Int? = null,
    val pageCount: Int? = null,
    val fileSize: Long? = null,
    val pixelWidth: Int? = null,
    val pixelHeight: Int? = null,
)

private fun dump(segment: MessageSegment): SegmentDump = SegmentDump(
    type = segment.type,
    content = segment.content,
    toolName = segment.toolName,
    toolArgs = segment.toolArgs,
    toolResult = segment.toolResult,
    toolCallId = segment.toolCallId,
    signature = segment.signature,
    signatureProvider = segment.signatureProvider,
    toolTranscription = segment.toolTranscription,
    errorCode = segment.errorCode,
    responseOutputItems = segment.responseOutputItems.size,
    responseOutputItemProvider = segment.responseOutputItemProvider,
)

private fun dump(call: ToolCallData): ToolCallDump = ToolCallDump(
    toolName = call.toolName,
    arguments = call.arguments,
    result = call.result,
    signature = call.signature,
    toolCallId = call.toolCallId,
    displayName = call.displayName,
    resultText = call.resultText,
    structuredResult = call.structuredResult,
    transcription = call.transcription,
    responseOutputItems = call.responseOutputItems.size,
    responseOutputItemProvider = call.responseOutputItemProvider,
)

private fun dump(meta: AttachmentMeta): List<AttachmentItemDump> = meta.items.map { item ->
    AttachmentItemDump(
        type = item.type,
        imageIndex = item.imageIndex,
        pageCount = item.pageCount,
        fileSize = item.fileSize,
        pixelWidth = item.pixelWidth,
        pixelHeight = item.pixelHeight,
    )
}

// -- scenario -> model ------------------------------------------------------------------------------

private fun buildMessage(scenario: ScenarioMessage): ChatMessage = ChatMessage(
    id = scenario.id,
    parentId = scenario.parentId,
    text = scenario.text,
    images = scenario.images,
    thoughts = scenario.thoughts,
    thoughtTitle = scenario.thoughtTitle,
    tokenCount = scenario.tokenCount,
    tokenUsage = null,
    status = MessageStatus.valueOf(scenario.status),
    participant = when (scenario.participant) {
        "USER" -> Participant.USER
        "MODEL" -> Participant.MODEL
        "ERROR" -> Participant.ERROR
        else -> error("unknown participant: ${scenario.participant}")
    },
    timestamp = scenario.timestamp,
    thoughtTimeMs = null,
    modelName = scenario.modelName,
    toolCall = scenario.toolCall?.let { call ->
        ToolCallData(
            toolName = call.toolName,
            arguments = call.arguments,
            result = call.result,
            signature = call.signature,
            toolCallId = call.toolCallId,
            resultImages = emptyList(),
            displayName = call.displayName,
            resultText = call.resultText,
            structuredResult = call.structuredResult,
            responseOutputItems = call.responseOutputItems,
            responseOutputItemProvider = call.responseOutputItemProvider,
            transcription = call.transcription,
        )
    },
    segments = scenario.segments?.map { segment ->
        MessageSegment(
            type = segment.type,
            content = segment.content,
            toolName = segment.toolName,
            toolArgs = segment.toolArgs,
            toolResult = segment.toolResult,
            toolCallId = segment.toolCallId,
            signature = segment.signature,
            signatureProvider = segment.signatureProvider,
            durationMs = segment.durationMs,
            toolState = segment.toolState,
            toolProgress = segment.toolProgress,
            toolTarget = segment.toolTarget,
            toolDisplayName = segment.toolDisplayName,
            toolResultText = segment.toolResultText,
            toolStructuredResult = segment.toolStructuredResult,
            toolImages = emptyList(),
            toolTranscription = segment.toolTranscription,
            responseOutputItems = segment.responseOutputItems,
            responseOutputItemProvider = segment.responseOutputItemProvider,
            errorCode = segment.errorCode,
        )
    },
    attachmentMeta = scenario.attachmentMeta?.let { meta ->
        AttachmentMeta(meta.items.map { item ->
            AttachmentItem(
                type = item.type,
                imageIndex = item.imageIndex,
                pageCount = item.pageCount,
                fileSize = item.fileSize,
                pixelWidth = item.pixelWidth,
                pixelHeight = item.pixelHeight,
            )
        })
    },
    retryText = null,
    runId = scenario.runId,
    runSequence = scenario.runSequence,
    consumedAtPass = null,
    source = null,
    displayPageId = null,
    markdownImages = emptyMap(),
    preparedMarkdown = emptyMap(),
    preparedMarkdownBytes = 0,
)

/** ToolDefinition construction mirrors the app's programmatic shape (tool settings / MCP). */
private fun buildTools(rawTools: List<JsonObject>?): List<ToolDefinition>? = rawTools?.map { raw ->
    val functionJson = raw["function"]?.jsonObject ?: error("tool without function")
    ToolDefinition(
        type = (raw["type"] as? JsonPrimitive)?.content ?: "function",
        function = ToolFunction(
            name = (functionJson["name"] as? JsonPrimitive)?.content ?: error("tool without name"),
            description = (functionJson["description"] as? JsonPrimitive)?.content ?: "",
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
    val required = json["required"]?.let { element ->
        (element as? JsonArray)?.mapNotNull { (it as? JsonPrimitive)?.content }
    } ?: emptyList()
    return ToolParameters(
        type = (json["type"] as? JsonPrimitive)?.content ?: "object",
        properties = properties,
        required = required,
        schema = json["schema"]?.jsonObject,
    )
}

// -- main ------------------------------------------------------------------------------------------

private fun costModelFor(scenario: Scenario): ContextCostModel =
    scenario.costModel?.let { ContextCostModels.forModel(it) } ?: ContextCostModel.Default

/** The error-detail formatter is caller-injected in the app; the stand pins a fixed shape. */
private fun generationErrorFormatter(detail: String): String = "formatted:$detail"

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: MessagePreparationTraceDump <scenario.json> <vocab.tiktoken>" }
    // The real app counts through the shipped o200k vocabulary once it is in memory; the stand
    // loads the same asset file so family cost models price with the real Kotlin BPE counter.
    O200kBase.loadFromFile(args[1])

    val scenario = traceJson.decodeFromString(Scenario.serializer(), File(args[0]).readText())
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }
    fun dumpMessages(messages: List<ChatMessage>) {
        messages.forEachIndexed { index, message ->
            line(
                "msg",
                index.toString(),
                esc(message.id),
                esc(message.parentId ?: "-"),
                esc(message.participant.name),
                esc(message.status.name),
                esc(message.text),
                esc(message.images.joinToString(",")),
                esc(message.modelName ?: "-"),
                esc(message.thoughts ?: "-"),
                message.timestamp.toString(),
                esc(message.runId ?: "-"),
                message.runSequence?.toString() ?: "-",
                esc(
                    traceJson.encodeToString(
                        ListSerializer(SegmentDump.serializer()),
                        message.segments.orEmpty().map(::dump),
                    ),
                ),
                message.toolCall?.let { esc(traceJson.encodeToString(ToolCallDump.serializer(), dump(it))) }
                    ?: "-",
                esc(traceJson.encodeToString(ListSerializer(AttachmentItemDump.serializer()), message.attachmentMeta?.let(::dump) ?: emptyList())),
            )
        }
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    try {
        val messages = scenario.messages.map(::buildMessage)
        val costs = costModelFor(scenario)
        when (scenario.mode) {
            "prepare" -> dumpMessages(
                prepareMessages(
                    messages = messages,
                    contextTokenBudget = scenario.budget,
                    includeAssistantReasoning = scenario.includeAssistantReasoning,
                    costs = costs,
                ),
            )
            "compactProjection" -> dumpMessages(applyNearestContextCompact(messages))
            "canonical" -> dumpMessages(canonicalContextMessages(messages))
            "strip" -> dumpMessages(stripEmptyTurns(messages))
            "merge" -> dumpMessages(mergeConsecutiveSameRole(messages))
            "validateTools" -> dumpMessages(validateToolMessages(messages))
            "limit" -> dumpMessages(
                limitContext(
                    messages = messages,
                    contextTokenBudget = scenario.budget,
                    includeAssistantReasoning = scenario.includeAssistantReasoning,
                    costs = costs,
                ),
            )
            "units" -> protocolAtomicUnits(messages).forEach { unit ->
                line("unit", esc(unit.joinToString(",") { it.id }))
            }
            "usage" -> {
                val usage = contextWindowUsage(
                    messages = messages,
                    tokenBudget = scenario.budget,
                    fixedTokenCost = scenario.fixedTokenCost,
                    includeAssistantReasoning = scenario.includeAssistantReasoning,
                    fixedComposition = FixedContextComposition(
                        systemPromptTokens = scenario.fixedSystemPromptTokens,
                        toolTokens = scenario.fixedToolTokens,
                    ),
                    costs = costs,
                )
                line(
                    "usage",
                    usage.estimatedTokenCount.toString(),
                    usage.tokenBudget.toString(),
                    usage.logicalMessageCount.toString(),
                    if (usage.hasCompactBoundary) "1" else "0",
                    usage.systemPromptTokens.toString(),
                    usage.toolTokens.toString(),
                    usage.messageTokens.toString(),
                    usage.progress.toString(),
                )
            }
            "retained" -> line(
                "retained",
                esc(
                    contextWindowRetainedMessageIds(
                        messages = messages,
                        tokenBudget = scenario.budget,
                        fixedTokenCost = scenario.fixedTokenCost,
                        includeAssistantReasoning = scenario.includeAssistantReasoning,
                        costs = costs,
                    ).joinToString(","),
                ),
            )
            "splitCompact" -> {
                val split = splitContextForCompactRetention(messages, scenario.retainMessages)
                line("prefix", esc(split.prefix.joinToString(",") { it.id }))
                line("retained", esc(split.retained.joinToString(",") { it.id }))
                line("count", split.retainedMessageCount.toString())
            }
            "splitLogical" -> {
                val split = splitLogicalContext(messages, scenario.retainLogicalMessages)
                line("prefix", esc(split.prefix.joinToString(",") { it.id }))
                line("suffix", esc(split.suffix.joinToString(",") { it.id }))
                line("count", split.logicalMessageCount.toString())
            }
            "estimate" -> line(
                "estimate",
                CostModelContextEstimator(costs)
                    .estimate(messages, includeAssistantReasoning = scenario.includeAssistantReasoning)
                    .toString(),
            )
            "estimateFixed" -> line(
                "fixed",
                CostModelContextEstimator(costs).estimateFixed(
                    systemPrompt = scenario.systemPrompt,
                    tools = buildTools(scenario.tools).orEmpty(),
                    initialUserPrompt = scenario.initialUserPrompt,
                    codeExecutionEnabled = scenario.codeExecutionEnabled,
                    googleSearchEnabled = scenario.googleSearchEnabled,
                    openAiWebSearchEnabled = scenario.openAiWebSearchEnabled,
                ).toString(),
            )
            "fixedComposition" -> {
                val composition = CostModelContextEstimator(costs).estimateFixedComposition(
                    systemPrompt = scenario.systemPrompt,
                    tools = buildTools(scenario.tools).orEmpty(),
                    initialUserPrompt = scenario.initialUserPrompt,
                    codeExecutionEnabled = scenario.codeExecutionEnabled,
                    googleSearchEnabled = scenario.googleSearchEnabled,
                    openAiWebSearchEnabled = scenario.openAiWebSearchEnabled,
                )
                line(
                    "fixedc",
                    composition.systemPromptTokens.toString(),
                    composition.toolTokens.toString(),
                )
            }
            "estimateText" -> line("text", CostModelContextEstimator(costs).estimateText(scenario.text).toString())
            "projectAssistantImages" -> dumpMessages(
                projectAssistantImagesToLatestUserMessage(messages, scenario.includeImages),
            )
            "projectToolResultImages" -> dumpMessages(
                projectToolResultImagesToUserMessage(messages, scenario.includeImages),
            )
            "projectStatuses" -> dumpMessages(
                projectGenerationStatusesForApi(messages, ::generationErrorFormatter),
            )
            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (error: Exception) {
        // No format gate here: only the exception class is part of the contract.
        line("error", error.javaClass.simpleName ?: "unknown")
    }
    print(out)
}
