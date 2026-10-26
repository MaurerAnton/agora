// Golden trace driver for util/ConchHandshake.kt.
//
// ConchHandshake.kt compiles VERBATIM from the app tree. Only the java.nio.charset.Charsets
// surface is substituted (platform/). The stand exercises the verify() function
// with deterministic inputs and freezes the output trace.
package conch.handshake.golden

import java.nio.charset.Charsets
import com.newoether.agora.util.ConchHandshake
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.put

private val json = Json { ignoreUnknownKeys = true }
private val scenarioJson = Json { ignoreUnknownKeys = true }

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = java.io.File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = java.io.File(path)
            java.io.File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(java.io.File(args[0])))
}

fun render(scenarioFile: java.io.File): String {
    val element = scenarioJson.parseToJsonElement(scenarioFile.readText())
    val scenario = (element as? JsonObject) ?: error("scenario is not a JsonObject")
    val op = scenario.string("op")

    return when (op) {
        "verify" -> {
            val apiKey = scenario.string("api_key")
            val expectedChallenge = scenario.string("expected_challenge")
            val handshake = scenario.jsonObject("handshake")

            val handshakeObj = ConchHandshake.decode(handshake.toString())
            val valid = handshakeObj.verify(apiKey, expectedChallenge)

            buildJsonObject {
                put("valid", valid)
            }.toString()
        }
        else -> error("unknown op $op")
    }
}

private fun JsonObject.string(name: String): String = (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")
private fun JsonObject.jsonObject(name: String): JsonObject = this[name] as JsonObject ?: error("missing object $name")