// Golden trace driver for BoundedSemanticEmbeddingSelector, used to pin the C++ port.
//
// EmbeddingSearchRow.kt and BoundedSemanticEmbeddingSelector.kt compile VERBATIM from the app
// tree; the driver calls the REAL selector through its suspend entry point — no transcription.
// The C++ core must reproduce every trace byte for byte.
//
// Scenario format: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package selector.golden

import com.newoether.agora.data.local.EmbeddingSearchRow
import com.newoether.agora.tool.BoundedSemanticEmbeddingSelector
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import java.io.File
import java.nio.ByteBuffer
import java.nio.ByteOrder

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

/** One keyset row as the scenario describes it; `dimension = -1` means "the query's size". */
@Serializable
private data class ScenarioRow(
    val id: Long = 0,
    @SerialName("message_id") val messageId: String = "",
    val dimension: Int = -1,
    val embedding: List<Float> = emptyList(),
)

@Serializable
private data class Scenario(
    val name: String,
    @SerialName("page_size") val pageSize: Int = 256,
    val query: List<Float> = emptyList(),
    val threshold: Float = 0f,
    val limit: Int = 5,
    /** Flat keyset rows; the loader pages them the way the database would. */
    val rows: List<ScenarioRow> = emptyList(),
    /** Verbatim pages, returned regardless of `afterId` — drives the ordering checks. */
    @SerialName("raw_pages") val rawPages: List<List<ScenarioRow>> = emptyList(),
    /** keyset | raw | oversized */
    val loader: String = "keyset",
)

/** The app stores vectors as big-endian IEEE-754; the scorer decodes them byte by byte. */
private fun encodeFloats(values: List<Float>): ByteArray =
    ByteBuffer.allocate(values.size * Float.SIZE_BYTES)
        .order(ByteOrder.BIG_ENDIAN)
        .apply { values.forEach { putFloat(it) } }
        .array()

private fun ScenarioRow.toRow(querySize: Int): EmbeddingSearchRow {
    val declared = if (dimension < 0) querySize else dimension
    return EmbeddingSearchRow(
        id = id,
        messageId = messageId,
        embedding = encodeFloats(embedding),
        dimension = declared,
    )
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: SelectorTraceDump <scenario.json> <unusedDir>" }
    val scenario = traceJson.decodeFromString(Scenario.serializer(), File(args[0]).readText())
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }

    val out = StringBuilder()
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|")).append('\n')
    }

    line("name", esc(scenario.name))
    line("page_size", scenario.pageSize.toString())
    line("threshold", scenario.threshold.toString())
    line("limit", scenario.limit.toString())
    line("query", scenario.query.joinToString(",") { it.toString() })

    try {
        val selector = BoundedSemanticEmbeddingSelector(scenario.pageSize)
        var call = 0
        val selection = runBlocking {
            selector.select(
                queryEmbedding = scenario.query.toFloatArray(),
                threshold = scenario.threshold,
                limit = scenario.limit,
                loadPage = { afterId, limit ->
                    when (scenario.loader) {
                        // Returns the pages verbatim, ignoring the keyset cursor — this is how the
                        // strictly-ordered paging contract gets exercised.
                        "raw" -> scenario.rawPages.getOrElse(call++) { emptyList() }
                            .map { it.toRow(scenario.query.size) }
                        "oversized" -> scenario.rows.dropWhile { it.id <= afterId }
                            .take(limit + 1)
                            .map { it.toRow(scenario.query.size) }
                        else -> scenario.rows.dropWhile { it.id <= afterId }
                            .take(limit)
                            .map { it.toRow(scenario.query.size) }
                    }
                },
            )
        }
        line("candidates", selection.candidates.size.toString())
        for (candidate in selection.candidates) {
            line("candidate", candidate.rowId.toString(), esc(candidate.messageId), candidate.score.toString())
        }
        line("scanned", selection.scannedRows.toString())
        line("skipped", selection.skippedInvalidRows.toString())
        line("best_score", selection.bestScore.toString())
        line("max_retained", selection.maxRetainedCandidates.toString())
    } catch (e: Throwable) {
        line("error", esc(e.message ?: e::class.simpleName ?: "error"))
    }
    print(out)
}
