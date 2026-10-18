// Golden trace driver for the shell tool definition builder, used to pin the C++ port.
//
// ShellToolDefinitions.kt compiles VERBATIM from the app tree against two stand-only leaf stubs
// (GenerationContext, ShellDeviceConfig) and the real maxWaitMs helper extracted from
// ShellDurableJobExecutor.kt. The driver calls build() directly — no transcription.
//
// Scenario format: see README.md in this directory. Build/run: see run_golden.sh.
package shell.definitions.golden

import com.newoether.agora.api.ToolDefinition
import com.newoether.agora.data.ShellDeviceConfig
import com.newoether.agora.tool.ShellToolDefinitions
import com.newoether.agora.viewmodel.GenerationContext
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
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

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "definitions",
    val shellEnabled: Boolean = true,
    val sandboxEnabled: Boolean = false,
    val toolTimeoutMs: Int = 60_000,
    val devices: List<ScenarioDevice> = emptyList(),
)

@Serializable
private data class ScenarioDevice(
    val name: String,
    val type: String = "conch",
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ShellDefinitionsTraceDump <scenario.json> <unusedRoot>" }
    val scenario = traceJson.decodeFromString(
        Scenario.serializer(),
        File(args[0]).readText(),
    )
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }
    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("mode", esc(scenario.mode))
    when (scenario.mode) {
        "definitions" -> {
            val context = GenerationContext(
                shellEnabled = scenario.shellEnabled,
                shellDevices = scenario.devices.map { ShellDeviceConfig(it.name, it.type) },
                sandboxEnabled = scenario.sandboxEnabled,
                toolTimeoutMs = scenario.toolTimeoutMs,
            )
            val tools = ShellToolDefinitions.build(context)
            line("count", tools.size.toString())
            for (tool in tools) {
                line("tool", esc(traceJson.encodeToString(ToolDefinition.serializer(), tool)))
            }
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}