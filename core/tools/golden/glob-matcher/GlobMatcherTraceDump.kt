// Golden trace driver for the portable path-glob matcher, used to pin the C++ port.
//
// PortableGlobMatcher.kt compiles VERBATIM from the app tree (it has no imports and no Android
// types) and the driver calls it directly — no transcription. The C++ core must reproduce the
// same trace byte for byte.
//
// Scenario format: {"name", "mode": "match", "pattern", "path"}. Trace format: see README.md.
// Build/run: see run_golden.sh in this directory.
package glob.matcher.golden

import com.newoether.agora.util.PortableGlobMatcher
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
    val mode: String = "match",
    val pattern: String = "",
    val path: String = "",
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: GlobMatcherTraceDump <scenario.json> <unusedRoot>" }
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
        "match" -> line("match", if (PortableGlobMatcher.matches(scenario.pattern, scenario.path)) "1" else "0")
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}