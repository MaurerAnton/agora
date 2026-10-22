// Golden trace driver for api/util/MalformedToolCall.kt (lifted verbatim with a widened
// visibility so the driver can call it).
//
// The random UUID suffix is masked in the trace (the port takes the entropy as a parameter), so
// the two sides stay comparable.
package malformed.tool.call.golden

import com.newoether.agora.api.util.malformedToolCallRequest
import com.newoether.agora.api.util.malformedToolCallResultText
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

private val UUID_MASK = Regex("[0-9a-f]{32}")

private fun mask(value: String): String = UUID_MASK.replace(value, "<uuid>")

private fun renderRequest(
    cause: String,
    originalName: String?,
    originalArguments: String?,
    streamKey: String?,
    signature: String?,
): JsonObject {
    val request = malformedToolCallRequest(cause, originalName, originalArguments, streamKey, signature)
    return buildJsonObject {
        put("id", mask(request.id))
        put("name", request.name)
        put("arguments", request.arguments)
        put("signature", request.signature)
        put("streamKey", mask(request.streamKey))
    }
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
    val trace = buildJsonObject {
        val requests = (root["requests"] as? kotlinx.serialization.json.JsonArray)?.map { element ->
            val spec = element.jsonObject
            renderRequest(
                cause = (spec["cause"] as JsonPrimitive).content,
                originalName = (spec["originalName"] as? JsonPrimitive)?.contentOrNull,
                originalArguments = (spec["originalArguments"] as? JsonPrimitive)?.contentOrNull,
                streamKey = (spec["streamKey"] as? JsonPrimitive)?.contentOrNull,
                signature = (spec["signature"] as? JsonPrimitive)?.contentOrNull,
            )
        } ?: emptyList()
        put("requests", kotlinx.serialization.json.buildJsonArray {
            requests.forEach { add(it) }
        })
        val results = (root["results"] as? kotlinx.serialization.json.JsonArray)?.map { element ->
            malformedToolCallResultText((element as JsonPrimitive).content)
        } ?: emptyList()
        put("results", kotlinx.serialization.json.buildJsonArray {
            results.forEach { add(kotlinx.serialization.json.JsonPrimitive(it)) }
        })
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
