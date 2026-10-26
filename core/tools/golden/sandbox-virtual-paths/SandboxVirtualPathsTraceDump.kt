// Golden trace driver for the pure sandbox virtual-path helpers in
// app/src/fdroid/java/com/newoether/agora/sandbox/SandboxVirtualPaths.kt.
//
// SandboxVirtualPaths.kt and PortableGlobMatcher.kt compile verbatim from the app tree; no
// platform substitute is needed. The driver pins path normalization and the root-anchored glob
// filter used by the sandbox file tools.
package sandbox.virtual.paths.golden

import com.newoether.agora.sandbox.globMatch
import com.newoether.agora.sandbox.normalizeVirtualPath
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

private val json = Json

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
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
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    return buildJsonObject {
        put("op", op)
        when (op) {
            "normalize" -> {
                val path = scenario.string("path")
                put("path", path)
                put("outcome", buildJsonObject {
                    put("normalized", normalizeVirtualPath(path))
                })
            }
            "glob" -> {
                val files = scenario.strings("files")
                val pattern = scenario.string("pattern")
                put("files", stringsJson(files))
                put("pattern", pattern)
                put("outcome", buildJsonObject {
                    put("files", stringsJson(globMatch(files, pattern)))
                })
            }
            else -> error("unknown op $op")
        }
    }.toString()
}

private fun stringsJson(values: List<String>): JsonArray = buildJsonArray {
    for (value in values) add(value)
}

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { it.jsonPrimitive.content }
}
