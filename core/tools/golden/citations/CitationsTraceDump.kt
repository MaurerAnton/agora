// Golden trace driver for the citation pipeline, used to pin the C++ port.
//
// Runs the REAL Kotlin sources (model.CitationPolicy + the Anthropic and Gemini citation
// projections, compiled straight from the app tree) over canned JSON scenarios and prints a
// canonical trace. The C++ core must reproduce the same trace byte for byte.
//
// The `uri` and `safeUrlFuzz` modes additionally pin java.net.URI itself (the JDK the reference
// runs on), because CitationPolicy.safeHttpUrl is defined in terms of it.
//
// Scenario and trace formats: see README.md in this directory.
// Build/run: see run_golden.sh in this directory.
package citations.golden

import com.newoether.agora.api.anthropic.toAnthropicCitationRecord
import com.newoether.agora.api.gemini.toGeminiCitations
import com.newoether.agora.model.CitationAnchor
import com.newoether.agora.model.CitationPolicy
import com.newoether.agora.model.CitationRecord
import com.newoether.agora.model.matchesCitationTitle
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import java.io.File
import java.net.URI

private val traceJson = Json { ignoreUnknownKeys = true }

/** Field-safe escaping shared with the C++ trace writer. */
private fun esc(value: String): String = buildString {
    for (c in value) {
        when (c) {
            '\\' -> append("\\\\")
            '|' -> append("\\p")
            ';' -> append("\\s")
            '"' -> append("\\q")
            '\n' -> append("\\n")
            '\r' -> append("\\r")
            '\t' -> append("\\t")
            else -> append(c)
        }
    }
}

private fun q(value: String): String = "\"" + esc(value) + "\""

private fun qn(value: String?): String = value?.let(::q) ?: "null"

// -- scenario shape -----------------------------------------------------------------------------------

@Serializable
private data class Scenario(
    val name: String,
    val mode: String,
    /** Policy modes: the inputs of CitationPolicy.create / normalize. */
    val provider: String = "",
    val kind: String = "",
    val title: String? = null,
    val url: String? = null,
    val fileName: String? = null,
    val location: String? = null,
    val providerSourceId: String? = null,
    val excerpt: String? = null,
    val anchors: List<ScenarioAnchor> = emptyList(),
    val answerText: String? = null,
    /** merge / deduplicate inputs. */
    val version: Int = 1,
    val sourceId: String = "",
    val incoming: ScenarioRecord? = null,
    val records: List<ScenarioRecord> = emptyList(),
    /** encode: record to encode; decode: raw string to decode. */
    val raw: String = "",
    /** matchTitle: the query. */
    val query: String = "",
    /** strip / copy: the source text. */
    val text: String = "",
    /** Projection modes: the answer text, the citation element / grounding metadata JSON. */
    val answerStartIndex: Int = 0,
    val citation: String = "",
    val metadata: String = "",
    /** uri / safeUrlFuzz: the case corpus batch. */
    val cases: List<String> = emptyList(),
)

@Serializable
private data class ScenarioAnchor(
    val startIndex: Int = 0,
    val endIndex: Int = 0,
    val citedText: String = "",
)

@Serializable
private data class ScenarioRecord(
    val version: Int = 1,
    val sourceId: String = "",
    val provider: String = "",
    val kind: String = "",
    val title: String = "",
    val url: String? = null,
    val fileName: String? = null,
    val location: String? = null,
    val providerSourceId: String? = null,
    val excerpt: String? = null,
    val anchors: List<ScenarioAnchor> = emptyList(),
)

private fun ScenarioAnchor.toModel() = CitationAnchor(startIndex, endIndex, citedText)

private fun ScenarioRecord.toModel() = CitationRecord(
    version = version,
    sourceId = sourceId,
    provider = provider,
    kind = kind,
    title = title,
    url = url,
    fileName = fileName,
    location = location,
    providerSourceId = providerSourceId,
    excerpt = excerpt,
    anchors = anchors.map(ScenarioAnchor::toModel),
)

// -- canonical record rendering -------------------------------------------------------------------------

private fun anchorField(anchor: CitationAnchor): String =
    "{" + anchor.startIndex + "," + anchor.endIndex + "," + q(anchor.citedText) + "}"

private fun recordFields(record: CitationRecord): String = listOf(
    "v=" + record.version,
    "sourceId=" + q(record.sourceId),
    "provider=" + q(record.provider),
    "kind=" + q(record.kind),
    "title=" + q(record.title),
    "url=" + qn(record.url),
    "fileName=" + qn(record.fileName),
    "location=" + qn(record.location),
    "providerSourceId=" + qn(record.providerSourceId),
    "excerpt=" + qn(record.excerpt),
    "anchors=[" + record.anchors.joinToString(",") { anchorField(it) } + "]",
).joinToString(";")

private fun recordsField(records: List<CitationRecord>): String =
    "[" + records.joinToString(",") { recordFields(it) } + "]"

// -- main -----------------------------------------------------------------------------------------------

fun main(args: Array<String>) {
    require(args.size == 1) { "usage: CitationsTraceDump <scenario.json>" }
    val scenario = traceJson.decodeFromString(Scenario.serializer(), File(args[0]).readText())
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
            "create" -> line(
                "create",
                CitationPolicy.create(
                    provider = scenario.provider,
                    kind = scenario.kind,
                    title = scenario.title,
                    url = scenario.url,
                    fileName = scenario.fileName,
                    location = scenario.location,
                    providerSourceId = scenario.providerSourceId,
                    excerpt = scenario.excerpt,
                    anchors = scenario.anchors.map(ScenarioAnchor::toModel),
                    answerText = scenario.answerText,
                )?.let(::recordFields) ?: "null",
            )

            "normalize" -> line(
                "normalize",
                CitationPolicy.normalize(scenario.toModel(), scenario.answerText)?.let(::recordFields) ?: "null",
            )

            "merge" -> {
                val first = scenario.toModel()
                val incoming = requireNotNull(scenario.incoming) { "merge requires incoming" }.toModel()
                line("merge", recordFields(CitationPolicy.merge(first, incoming)))
            }

            "deduplicate" -> {
                val deduped = CitationPolicy.deduplicate(scenario.records.map(ScenarioRecord::toModel), scenario.answerText)
                line("count", deduped.size.toString())
                deduped.forEachIndexed { index, record -> line("record", index.toString(), recordFields(record)) }
            }

            "strip" -> line("strip", q(CitationPolicy.stripPrivateMarkers(scenario.text)))

            "copy" -> line(
                "copy",
                q(CitationPolicy.copyText(scenario.text, scenario.records.map(ScenarioRecord::toModel))),
            )

            "encode" -> line("encode", q(CitationPolicy.encode(scenario.toModel())))

            "decode" -> line(
                "decode",
                CitationPolicy.decode(scenario.raw, scenario.answerText)?.let(::recordFields) ?: "null",
            )

            "safeUrl" -> line("safeUrl", qn(CitationPolicy.safeHttpUrl(scenario.url)))

            "matchTitle" -> line(
                "matchTitle",
                if (scenario.records.map(ScenarioRecord::toModel).matchesCitationTitle(scenario.query)) "1" else "0",
            )

            "anthropicCitation" -> {
                val parsed = runCatching { traceJson.parseToJsonElement(scenario.citation) }
                val element = parsed.getOrNull()
                if (element == null) {
                    line("parse", "throw", parsed.exceptionOrNull()!!.javaClass.simpleName)
                } else {
                    val record = element.toAnthropicCitationRecord(scenario.answerStartIndex, scenario.text)
                    line("record", record?.let(::recordFields) ?: "null")
                }
            }

            "geminiCitations" -> {
                val parsed = runCatching { traceJson.parseToJsonElement(scenario.metadata) }
                val element = parsed.getOrNull()
                if (element == null) {
                    line("parse", "throw", parsed.exceptionOrNull()!!.javaClass.simpleName)
                } else if (element !is JsonObject) {
                    line("metadata", "notObject")
                } else {
                    val records = element.toGeminiCitations(scenario.text)
                    line("count", records.size.toString())
                    records.forEachIndexed { index, record -> line("record", index.toString(), recordFields(record)) }
                }
            }

            "uri" -> scenario.cases.forEachIndexed { index, raw ->
                line("case", index.toString(), q(raw))
                val parsed = runCatching { URI(raw) }
                val uri = parsed.getOrNull()
                if (uri == null) {
                    line("parse", "throw", parsed.exceptionOrNull()!!.javaClass.simpleName)
                } else {
                    line(
                        "parse",
                        "scheme=" + qn(uri.scheme),
                        "userInfo=" + qn(uri.userInfo),
                        "host=" + qn(uri.host),
                        "port=" + uri.port.toString(),
                        "path=" + qn(uri.path),
                        "query=" + qn(uri.query),
                        "fragment=" + qn(uri.fragment),
                        "authority=" + qn(uri.authority),
                        "opaque=" + if (uri.isOpaque) "1" else "0",
                        "absolute=" + if (uri.isAbsolute) "1" else "0",
                    )
                    val ascii = runCatching { uri.toASCIIString() }
                    if (ascii.isSuccess) line("ascii", q(ascii.getOrThrow()))
                    else line("ascii", "throw", ascii.exceptionOrNull()!!.javaClass.simpleName)
                    val normalized = runCatching { uri.normalize() }
                    if (normalized.isSuccess) {
                        line("normalized", q(normalized.getOrThrow().toString()))
                        val normalizedAscii = runCatching { normalized.getOrThrow().toASCIIString() }
                        if (normalizedAscii.isSuccess) line("normalizedAscii", q(normalizedAscii.getOrThrow()))
                        else line("normalizedAscii", "throw", normalizedAscii.exceptionOrNull()!!.javaClass.simpleName)
                    } else {
                        line("normalized", "throw", normalized.exceptionOrNull()!!.javaClass.simpleName)
                    }
                }
            }

            "safeUrlFuzz" -> scenario.cases.forEachIndexed { index, raw ->
                line("case", index.toString(), q(raw), qn(CitationPolicy.safeHttpUrl(raw)))
            }

            else -> error("unknown mode: ${scenario.mode}")
        }
    } catch (error: Exception) {
        // No format gate in this module: the exception class alone is part of the contract.
        line("error", error.javaClass.simpleName ?: "unknown")
    }
    print(out)
}

private fun Scenario.toModel() = CitationRecord(
    version = version,
    sourceId = sourceId,
    provider = provider,
    kind = kind,
    title = title.orEmpty(),
    url = url,
    fileName = fileName,
    location = location,
    providerSourceId = providerSourceId,
    excerpt = excerpt,
    anchors = anchors.map(ScenarioAnchor::toModel),
)
