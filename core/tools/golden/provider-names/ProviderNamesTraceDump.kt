// Golden trace driver for the custom provider name policy, used to pin the C++ port.
//
// CustomProviderNamePolicy.kt and CustomProviderConfig.kt compile VERBATIM from the app tree and
// the driver calls them directly — no transcription. The C++ core must reproduce the same trace
// byte for byte.
//
// Scenario format: see README.md in this directory. Build/run: see run_golden.sh.
package provider.names.golden

import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.CustomProviderNamePolicy
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
    val mode: String = "allowed",
    val providerName: String = "",
    val existingNames: List<String> = emptyList(),
    val currentName: String? = null,
    val configs: List<String> = emptyList(),
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ProviderNamesTraceDump <scenario.json> <unusedRoot>" }
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
        "allowed" -> line("allowed", if (CustomProviderNamePolicy.isAllowed(scenario.providerName)) "1" else "0")
        "conflict" -> line(
            "conflict",
            if (CustomProviderNamePolicy.hasConflict(
                    scenario.providerName, scenario.existingNames, scenario.currentName,
                )
            ) "1" else "0",
        )
        "sanitize" -> {
            val result = CustomProviderNamePolicy.sanitize(
                scenario.configs.map { CustomProviderConfig(name = it) },
            )
            line("accepted", result.accepted.size.toString())
            result.accepted.forEachIndexed { index, config -> line("accept", index.toString(), esc(config.name)) }
            line("rejected", result.rejected.size.toString())
            result.rejected.forEachIndexed { index, config -> line("reject", index.toString(), esc(config.name)) }
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}