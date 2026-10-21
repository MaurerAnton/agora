// Golden trace driver for api/openai/OpenAiRequestValidator.kt (compiled verbatim from the app
// tree) plus the Responses request models extracted from api/LlmProvider.kt.
//
// ops:
//   "input"    -> toResponsesInput(messages, provider, targetModel) -> the JSON input array
//   "tools"    -> toResponsesTools(tools)                          -> the JSON tool array
//   "validate" -> requireValidWireFormat(request, provider)       -> {"ok":true} or the failure
package openai.responses.request.golden

import com.newoether.agora.api.OpenAiContentPart
import com.newoether.agora.api.OpenAiImageUrl
import com.newoether.agora.api.OpenAiMessage
import com.newoether.agora.api.OpenAiRequestFunction
import com.newoether.agora.api.OpenAiRequestToolCall
import com.newoether.agora.api.OpenAiResponseTool
import com.newoether.agora.api.OpenAiResponsesRequest
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.api.openai.toResponsesInput
import com.newoether.agora.api.openai.toResponsesTools
import com.newoether.agora.api.openai.requireValidWireFormat
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
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.encodeToJsonElement
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json { ignoreUnknownKeys = true }

/** The provider's request encoder (BaseOpenAiProvider): defaults are written, nulls are not. */
private val requestJson = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

private fun buildMessage(spec: JsonObject): OpenAiMessage {
    val role = (spec["role"] as JsonPrimitive).content
    val content = (spec["content"] as? JsonArray)?.map { element ->
        val part = element.jsonObject
        OpenAiContentPart(
            type = (part["type"] as JsonPrimitive).content,
            text = (part["text"] as? JsonPrimitive)?.contentOrNull,
            imageUrl = (part["image_url"] as? JsonObject)?.let { image ->
                OpenAiImageUrl(url = (image["url"] as JsonPrimitive).content)
            },
        )
    }
    val toolCalls = (spec["tool_calls"] as? JsonArray)?.map { element ->
        val call = element.jsonObject
        val function = call["function"]!!.jsonObject
        OpenAiRequestToolCall(
            id = (call["id"] as JsonPrimitive).content,
            type = (call["type"] as? JsonPrimitive)?.content ?: "function",
            function = OpenAiRequestFunction(
                name = (function["name"] as JsonPrimitive).content,
                arguments = (function["arguments"] as? JsonPrimitive)?.content ?: "",
            ),
        )
    }
    val outputItems = (spec["response_output_items"] as? JsonArray)?.map { it.jsonObject }
    return OpenAiMessage(
        role = role,
        content = content,
        toolCalls = toolCalls,
        toolCallId = (spec["tool_call_id"] as? JsonPrimitive)?.contentOrNull,
        reasoningContent = (spec["reasoning_content"] as? JsonPrimitive)?.contentOrNull,
        responseOutputItems = outputItems,
        responseOutputItemProvider = (spec["response_output_item_provider"] as? JsonPrimitive)?.contentOrNull,
        responseOutputItemModel = (spec["response_output_item_model"] as? JsonPrimitive)?.contentOrNull,
    )
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
    val op = (root["op"] as JsonPrimitive).content
    val trace: JsonObject = when (op) {
        "input" -> {
            val messages = (root["messages"] as? JsonArray)?.map { buildMessage(it.jsonObject) }
                ?: emptyList()
            val provider = (root["provider"] as? JsonPrimitive)?.contentOrNull
            val targetModel = (root["target_model"] as? JsonPrimitive)?.contentOrNull
            val input = messages.toResponsesInput(provider, targetModel)
            buildJsonObject { put("input", buildJsonArray { input.forEach { add(it) } }) }
        }
        "tools" -> {
            val tools = (root["tools"] as? JsonArray)?.map { element ->
                json.decodeFromJsonElement(ToolDefinition.serializer(), element)
            } ?: emptyList()
            val converted = tools.toResponsesTools()
            buildJsonObject {
                put("tools", buildJsonArray {
                    converted.forEach { tool ->
                        add(requestJson.encodeToJsonElement(OpenAiResponseTool.serializer(), tool))
                    }
                })
            }
        }
        "validate" -> {
            val request = json.decodeFromJsonElement(
                OpenAiResponsesRequest.serializer(),
                root["request"]!!,
            )
            val provider = (root["provider"] as? JsonPrimitive)?.content ?: "openai"
            try {
                request.requireValidWireFormat(provider)
                buildJsonObject { put("ok", true) }
            } catch (error: RequestFormatException) {
                buildJsonObject {
                    put("error", buildJsonObject {
                        put("type", "RequestFormatException")
                        put("message", error.message ?: "")
                        put("violations", buildJsonArray { error.violations.forEach { add(it) } })
                    })
                }
            }
        }
        else -> error("unknown op `$op`")
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
