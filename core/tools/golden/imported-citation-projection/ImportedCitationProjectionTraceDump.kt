// Golden trace driver for data/ImportedCitationProjection.kt, compiled verbatim from the app tree.
//
// ops:
//   "gpt"    -> projectChatGptCitations(rawText, references)  -> {text, citations[]}
//   "claude" -> projectClaudeCitation(citation, startIndex, answerText) -> {citation|null}
//   "encode" -> encodeImportedCitations(citations, answerText)          -> {encoded|null}
//
// CitationRecord is rendered field by field (all 11 fields, nulls included) so the C++ side can
// compare structurally. Input records for "encode" are decoded by the real @Serializable
// serializer, so no default is reimplemented in the driver.
package imported.citation.projection.golden

import com.newoether.agora.data.encodeImportedCitations
import com.newoether.agora.data.projectClaudeCitation
import com.newoether.agora.data.projectChatGptCitations
import com.newoether.agora.model.CitationRecord
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.decodeFromJsonElement
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import java.io.File

private val json = Json { ignoreUnknownKeys = true }

private fun CitationRecord.render(): JsonObject = buildJsonObject {
    put("version", version)
    put("sourceId", sourceId)
    put("provider", provider)
    put("kind", kind)
    put("title", title)
    put("url", url)
    put("fileName", fileName)
    put("location", location)
    put("providerSourceId", providerSourceId)
    put("excerpt", excerpt)
    put("anchors", buildJsonArray {
        anchors.forEach { anchor ->
            add(buildJsonObject {
                put("startIndex", anchor.startIndex)
                put("endIndex", anchor.endIndex)
                put("citedText", anchor.citedText)
            })
        }
    })
}

private fun renderGpt(rawText: String, references: List<JsonElement>): JsonObject {
    val projected = projectChatGptCitations(rawText, references)
    return buildJsonObject {
        put("text", projected.text)
        put("citations", buildJsonArray { projected.citations.forEach { add(it.render()) } })
    }
}

private fun renderClaude(citation: JsonElement, startIndex: Int, answerText: String): JsonObject {
    val record = projectClaudeCitation(citation, startIndex, answerText)
    return buildJsonObject {
        if (record == null) put("citation", JsonNull)
        else put("citation", record.render())
    }
}

fun main(args: Array<String>) {
    if (args.firstOrNull() == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String {
    val root = Json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = (root["op"] as? JsonPrimitive)?.contentOrNull ?: error("`op` is required")
    val trace: JsonObject = when (op) {
        "gpt" -> {
            val rawText = (root["rawText"] as? JsonPrimitive)?.contentOrNull ?: error("`rawText`")
            val references = (root["references"] as? JsonArray)?.toList() ?: emptyList()
            renderGpt(rawText, references)
        }
        "claude" -> {
            val citation = root["citation"] ?: error("`citation` is required")
            val startIndex = (root["startIndex"] as? JsonPrimitive)?.contentOrNull?.toInt() ?: 0
            val answerText = (root["answerText"] as? JsonPrimitive)?.contentOrNull ?: ""
            renderClaude(citation, startIndex, answerText)
        }
        "encode" -> {
            val records = json.decodeFromJsonElement<List<CitationRecord>>(
                root["citations"] ?: error("`citations` is required"),
            )
            val answerText = (root["answerText"] as? JsonPrimitive)?.contentOrNull ?: ""
            val encoded = encodeImportedCitations(records, answerText)
            buildJsonObject {
                if (encoded == null) put("encoded", JsonNull) else put("encoded", encoded)
            }
        }
        else -> error("unknown op `$op`")
    }
    return Json.encodeToString(JsonObject.serializer(), trace)
}
