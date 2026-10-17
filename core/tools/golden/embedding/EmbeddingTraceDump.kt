// Golden trace driver for the EmbeddingClient parsing/encoding, used to pin the C++ port.
//
// Runs the REAL @Serializable request models (extracted verbatim from EmbeddingClient.kt) over
// canned scenarios and prints a canonical trace. The response parsing is a line-for-line
// transcription of computeEmbedding/computeEmbeddings without the HTTP call (the transport
// boundary: url/headers/HttpClient and DebugLog stay in Kotlin). The C++ core must reproduce
// the same trace byte for byte.
//
// Scenario format: {"name", "mode": "requestSingle"|"requestBatch"|"single"|"batch", "text",
// "texts", "model", "response"}. Trace format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package embedding.golden

import com.newoether.agora.api.BatchEmbeddingRequest
import com.newoether.agora.api.EmbeddingRequest
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.float
import kotlinx.serialization.encodeToString
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
    val mode: String = "single",
    val text: String = "",
    val texts: List<String> = emptyList(),
    val model: String = "text-embedding-3-small",
    val response: String? = null,
)

/**
 * Transcription of computeEmbedding's response parsing: every structural failure (bad JSON,
 * non-object root, missing/incorrect `data`, missing `embedding`, a non-primitive element or a
 * non-numeric value) yields null, exactly like the surrounding try/catch.
 */
private fun parseSingle(response: String?): List<Float>? {
    if (response == null) return null
    return try {
        val parsed = traceJson.parseToJsonElement(response).jsonObject
        val data = parsed["data"]?.jsonArray ?: return null
        val embedding = data.firstOrNull()
            ?.jsonObject
            ?.get("embedding")
            ?.jsonArray
            ?: return null
        FloatArray(embedding.size) { i -> embedding[i].jsonPrimitive.float }.toList()
    } catch (e: Exception) {
        null
    }
}

/**
 * Transcription of computeEmbeddings' response parsing: the early paths and every exception
 * produce one null PER TEXT; a successful parse produces one entry per data item (the Kotlin
 * result length follows the response, not the request — pinned).
 */
private fun parseBatch(response: String?, count: Int): List<String?> {
    if (response == null) return List(count) { null }
    return try {
        val parsed = traceJson.parseToJsonElement(response).jsonObject
        val data = parsed["data"]?.jsonArray ?: return List(count) { null }
        data.map { item ->
            val embedding = item.jsonObject["embedding"]?.jsonArray ?: return@map null
            FloatArray(embedding.size) { i -> embedding[i].jsonPrimitive.float }
                .joinToString(",") { it.toString() }
        }
    } catch (e: Exception) {
        List(count) { null }
    }
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: EmbeddingTraceDump <scenario.json> <unusedRoot>" }
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
        "requestSingle" -> {
            line(
                "body",
                esc(traceJson.encodeToString(EmbeddingRequest(scenario.text, scenario.model))),
            )
        }
        "requestBatch" -> {
            line(
                "body",
                esc(traceJson.encodeToString(BatchEmbeddingRequest(scenario.texts, scenario.model))),
            )
        }
        "single" -> {
            val embedding = parseSingle(scenario.response)
            line("embedding", "0", embedding?.joinToString(",") { it.toString() } ?: "none")
        }
        "batch" -> {
            // computeEmbeddings returns early for empty input, before touching the response.
            if (scenario.texts.isNotEmpty()) {
                for ((index, entry) in parseBatch(scenario.response, scenario.texts.size).withIndex()) {
                    line("embedding", index.toString(), entry ?: "none")
                }
            }
        }
        else -> error("unknown mode: ${scenario.mode}")
    }
    print(out)
}