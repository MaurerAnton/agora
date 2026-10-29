// Golden trace driver for parseConchSseLines, used to pin the C++ port.
//
// ConchSseParser.kt compiles VERBATIM from the app tree; shellUtf8Prefix and its byte budget are
// lifted verbatim out of ShellClient.kt by `agora_extract shell-prefix` (that file as a whole pulls in
// transport and Android-only logging). The driver calls the REAL parser — no transcription —
// feeding it the scenario's raw SSE lines and recording everything it emitted along the way.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package conch.sse.golden

import com.newoether.agora.tool.parseConchSseLines
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.SerialName
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
    val encrypted: Boolean = false,
    /** The raw SSE lines handed to the parser; end of input is signalled by null. */
    val lines: List<String> = emptyList(),
    /** When set, the decrypt callback throws this message instead of returning. */
    @SerialName("decrypt_error") val decryptError: String? = null,
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ConchSseTraceDump <scenario.json> <unusedDir>" }
    val scenario = traceJson.decodeFromString(Scenario.serializer(), File(args[0]).readText())
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }

    val emitted = mutableListOf<String>()
    var cursor = 0

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("encrypted", if (scenario.encrypted) "1" else "0")

    try {
        val result = runBlocking {
            parseConchSseLines(
                encrypted = scenario.encrypted,
                readLine = { if (cursor < scenario.lines.size) scenario.lines[cursor++] else null },
                decrypt = { text ->
                    val failure = scenario.decryptError
                    if (failure != null) throw IllegalStateException(failure)
                    text
                },
                onOutput = { emitted += it },
            )
        }
        for (delta in emitted) line("emit", esc(delta))
        line("output", esc(result.output))
        line("exit_code", result.exitCode?.toString() ?: "-")
        line("error_message", result.errorMessage?.let { esc(it) } ?: "-")
        line("timed_out", if (result.timedOut) "1" else "0")
        line("warning", result.warningMessage?.let { esc(it) } ?: "-")
    } catch (e: Throwable) {
        // Streaming output that was already emitted stays part of the trace: the parser is
        // allowed to have flushed deltas before the frame that failed closed.
        for (delta in emitted) line("emit", esc(delta))
        line("error", esc(e.message ?: e::class.simpleName ?: "error"))
    }
    print(out)
}
