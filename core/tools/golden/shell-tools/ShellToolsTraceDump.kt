// Golden trace driver for the shell tool JSON helpers and the bounded file-read projection.
//
// Runs the REAL sources compiled from the app tree: ShellToolJson.kt and
// BoundedFileReadJson.kt verbatim, plus ShellFileReadResult/shellFileLineCount extracted
// verbatim from ShellClient.kt. The C++ core must reproduce the same trace byte for byte.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package shell.tools.golden

import com.newoether.agora.tool.arg
import com.newoether.agora.tool.boolArg
import com.newoether.agora.tool.boundedFileReadJson
import com.newoether.agora.tool.jsonError
import com.newoether.agora.tool.parseToolArgs
import com.newoether.agora.util.ShellFileReadResult
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File

private val traceJson = Json { ignoreUnknownKeys = true }

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
    val mode: String = "toolArgs",
    val arguments: String = "",
    val probeKey: String = "",
    val type: String = "",
    val message: String = "",
    val server: String? = null,
    val command: String? = null,
    val fileServer: String = "",
    val filePath: String = "",
    val fileContent: String = "",
    val fileLines: Int = 0,
    val totalLines: Int = 0,
    val totalBytes: Long = 0,
    val offset: Long = 0,
    val limit: Long = 0,
    val fileTruncated: Boolean = false,
    val maxLength: Int? = null,
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ShellToolsTraceDump <scenario.json> <unusedRoot>" }
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
    try {
        when (scenario.mode) {
            "toolArgs" -> {
                val parsed = parseToolArgs(scenario.arguments)
                for ((key, value) in parsed) {
                    line("arg", esc(key), esc(value.toString()))
                }
                line("probe", esc(arg(parsed, scenario.probeKey)))
                line("bool", if (boolArg(parsed, scenario.probeKey)) "1" else "0")
            }
            "jsonError" -> {
                line("json", esc(jsonError(scenario.type, scenario.message, scenario.server, scenario.command)))
            }
            "fileRead" -> {
                val result = ShellFileReadResult(
                    content = scenario.fileContent,
                    lines = scenario.fileLines,
                    totalLines = scenario.totalLines,
                    totalBytes = scenario.totalBytes,
                    returnedBytes = 0L,
                    offset = scenario.offset,
                    limit = scenario.limit,
                    truncated = scenario.fileTruncated,
                )
                val json = if (scenario.maxLength != null) {
                    boundedFileReadJson(scenario.fileServer, scenario.filePath, result, scenario.maxLength)
                } else {
                    boundedFileReadJson(scenario.fileServer, scenario.filePath, result)
                }
                line("json", esc(json))
            }
            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (error: Exception) {
        line("error", error.javaClass.simpleName ?: "unknown")
    }
    print(out)
}