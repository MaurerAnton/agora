// Golden trace driver for the provider support logic (HTTP error mapping, base-URL versioning,
// provider defaults), used to pin the C++ port.
//
// Runs the REAL Kotlin sources compiled straight from the app tree — ProviderHttpError.kt,
// BaseUrlResolver.kt, ProviderDefaults.kt — over canned JSON scenarios and prints a canonical
// trace. The C++ core must reproduce the same trace byte for byte.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package provider.support.golden

import com.newoether.agora.api.ProviderDefaults
import com.newoether.agora.api.parseProviderHttpErrorBody
import com.newoether.agora.api.providerHttpError
import com.newoether.agora.api.extractStructuredProviderHttpErrorMessage
import com.newoether.agora.api.BaseUrlResolver
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

private fun orNull(value: String?): String = value?.let { esc(it) } ?: "-"

@Serializable
private data class Scenario(
    val name: String,
    val mode: String = "httpError",
    val statusCode: Int = 404,
    val rawBody: String? = null,
    val url: String = "",
    val embeddingProvider: String = "",
    val compatibleName: String = "",
    val openAiMap: Map<String, String> = emptyMap(),
)

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: ProviderSupportTraceDump <scenario.json> <unusedRoot>" }
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
        "httpError" -> {
            val parsed = scenario.rawBody?.let { parseProviderHttpErrorBody(it) }
            if (parsed == null) {
                line("parse", "none")
            } else {
                line(
                    "parse",
                    "parsed",
                    "structured=${if (parsed.structured) 1 else 0}",
                    "code=${orNull(parsed.code)}",
                    "type=${orNull(parsed.type)}",
                    "message=${esc(parsed.message)}",
                )
            }
            val error = providerHttpError(scenario.statusCode, scenario.rawBody)
            line(
                "http",
                "api",
                "code=${orNull(error.code)}",
                "type=${orNull(error.type)}",
                "message=${esc(error.message)}",
            )
            line(
                "structured",
                orNull(scenario.rawBody?.let { extractStructuredProviderHttpErrorMessage(it) }),
            )
        }
        "baseUrl" -> {
            line("hasVersion", if (BaseUrlResolver.hasVersionSegment(scenario.url)) "1" else "0")
            line("withV1", esc(BaseUrlResolver.withV1(scenario.url)))
            line("withoutTrailing", orNull(BaseUrlResolver.withoutTrailingVersion(scenario.url)))
        }
        "defaults" -> {
            line("embedding", esc(ProviderDefaults.embeddingBaseUrl(scenario.embeddingProvider)))
            line(
                "compatible",
                if (ProviderDefaults.isOpenAiCompatibleEmbedding(scenario.compatibleName)) "1" else "0",
            )
            line("openAiCompatible", esc(ProviderDefaults.openAiCompatibleBaseUrl(scenario.openAiMap)))
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}