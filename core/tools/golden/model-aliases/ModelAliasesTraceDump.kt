// Golden trace driver for the model-alias inference, used to pin the C++ port.
//
// The alias block is extracted verbatim from CustomProviderIdentity.kt (alias regexes, the
// display-name tables and inferModelAlias with its Gemini/Claude/Nova branches); the driver calls
// it directly — no transcription. The C++ core must reproduce the same trace byte for byte.
//
// Scenario format: {"name", "mode": "alias", "modelName"}. Trace format: see README.md.
package model.aliases.golden

import com.newoether.agora.data.inferModelAlias
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
    val mode: String = "alias",
    val modelName: String = "",
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ModelAliasesTraceDump <scenario.json> <unusedRoot>" }
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
        "alias" -> line("alias", esc(inferModelAlias(scenario.modelName)))
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}
