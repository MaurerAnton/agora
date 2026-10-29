// Golden trace driver for shellCommandValidationError from ShellToolProvider.kt.
//
// ShellToolProvider.kt is a 763-line transport-coupled object. Only the pure-logic
// shellCommandValidationError function and its two constants are extracted verbatim
// by `agora_extract shell-validation`. The stand exercises the validation with deterministic inputs.
package shell.validation.golden

import com.newoether.agora.tool.shellCommandValidationError
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive

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
    val op = scenario.getString("op")

    return when (op) {
        "shellCommandValidationError" -> {
            val command = scenario.getString("command")
            val workdir = scenario.getString("workdir")
            val error = shellCommandValidationError(command, workdir)
            error ?: "<null>"
        }
        else -> error("unknown op $op")
    }
}

private fun JsonObject.getString(name: String): String = 
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")