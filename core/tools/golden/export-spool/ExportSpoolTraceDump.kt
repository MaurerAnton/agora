// Golden trace driver for data/ExportSpool.kt, compiled verbatim from the app tree.
//
// A scenario either replays a writer op sequence (conversation/run/message/loop/task writes plus
// finish -> the spool bytes and its ExportSpoolIndex) or plants raw spool content, then runs a
// list of reads against the same file:
//   {"kind":"records","slice":"conv:0"|"task:0"|[start,end]}                 -> readRecords
//   {"kind":"iterator","slices":[...],"type":"M","probeNext":true}           -> recordIterator
// Symbolic slices resolve against the finished index; explicit slices are byte ranges (read-only
// scenarios compute them from the planted content). Errors are pinned as {type,message}.
package export.spool.golden

import com.newoether.agora.data.ExportSpoolSlice
import com.newoether.agora.data.ExportSpoolReader
import com.newoether.agora.data.ExportSpoolWriter
import java.io.File
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private const val SPOOL_FILE = "platform_spool.tmp"

private data class ReadResult(
    val payloads: List<String> = emptyList(),
    val records: List<Pair<String, String>> = emptyList(),
    val error: Throwable? = null,
    val hasNextAfter: Boolean? = null,
    val errorAfter: Throwable? = null,
)

private fun errorJson(error: Throwable?): JsonElement? =
    if (error == null) null else buildJsonObject {
        put("type", error::class.simpleName ?: error.javaClass.name)
        put("message", error.message ?: "")
    }

private fun resolveSlice(element: JsonElement, index: com.newoether.agora.data.ExportSpoolIndex?): ExportSpoolSlice {
    val text = (element as? JsonPrimitive)?.contentOrNull
    if (text != null) {
        when {
            text.startsWith("conv:") -> {
                val position = text.removePrefix("conv:").toInt()
                return index!!.conversations[position].slice
            }
            text.startsWith("task:") -> {
                val position = text.removePrefix("task:").toInt()
                return index!!.taskSlices[position]
            }
        }
    }
    val range = (element as? JsonArray)?.toList() ?: error("bad slice")
    val start = (range[0] as JsonPrimitive).content.toInt()
    val end = (range[1] as JsonPrimitive).content.toInt()
    return ExportSpoolSlice(start.toLong(), end.toLong())
}

private fun runRead(reader: ExportSpoolReader, spec: JsonObject, index: com.newoether.agora.data.ExportSpoolIndex?): ReadResult {
    val kind = (spec["kind"] as JsonPrimitive).content
    val probeNext = (spec["probeNext"] as? JsonPrimitive)?.contentOrNull == "true"
    if (kind == "records") {
        val slice = resolveSlice(spec["slice"]!!, index)
        val records = mutableListOf<Pair<String, String>>()
        return try {
            reader.readRecords(slice) { type, json -> records += type to json }
            ReadResult(records = records)
        } catch (error: Throwable) {
            ReadResult(records = records, error = error)
        }
    }
    val sliceElements = spec["slices"]!!.let { it as JsonArray }.toList()
    val slices = sliceElements.map { resolveSlice(it, index) }
    val type = (spec["type"] as? JsonPrimitive)?.contentOrNull
    val iterator = reader.recordIterator(slices, type)
    val payloads = mutableListOf<String>()
    var error: Throwable? = null
    var hasNextAfter: Boolean? = null
    var errorAfter: Throwable? = null
    try {
        while (iterator.hasNext()) payloads += iterator.next()
    } catch (caught: Throwable) {
        error = caught
    }
    if (error != null) {
        try {
            hasNextAfter = iterator.hasNext()
        } catch (caught: Throwable) {
            errorAfter = caught
        }
    }
    if (probeNext) {
        try {
            payloads += iterator.next()
        } catch (caught: Throwable) {
            errorAfter = caught
        }
    }
    return ReadResult(payloads = payloads, error = error, hasNextAfter = hasNextAfter, errorAfter = errorAfter)
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
    val file = File(SPOOL_FILE)
    file.delete()

    var index: com.newoether.agora.data.ExportSpoolIndex? = null
    var writeError: Throwable? = null
    val writes = root["writes"] as? JsonArray
    if (writes != null) {
        val writer = ExportSpoolWriter(file)
        try {
            for (element in writes) {
                val op = element.jsonObject
                val name = (op["op"] as JsonPrimitive).content
                val json = (op["json"] as JsonPrimitive).content
                when (name) {
                    "conversation" -> writer.writeConversation(
                        (op["id"] as JsonPrimitive).content,
                        (op["dataChangedAt"] as JsonPrimitive).content.toLong(),
                        json,
                    )
                    "run" -> writer.writeRun(json)
                    "message" -> writer.writeMessage(json)
                    "loop" -> writer.writeLoop(json)
                    "task" -> writer.writeTask(json)
                    else -> error("unknown write op `$name`")
                }
            }
            index = writer.finish()
        } catch (caught: Throwable) {
            writeError = caught
            runCatching { writer.close() }
        }
        writer.close()
    } else {
        val content = (root["content"] as? JsonPrimitive)?.contentOrNull
        if (content != null) file.writeText(content, Charsets.UTF_8)
        else file.writeBytes(ByteArray(0))
    }

    val bytes = if (file.exists()) file.readText(Charsets.UTF_8) else ""

    // After an aborted writer the Kotlin export stops; symbolic slices would have no index to
    // resolve against, so reads are skipped and pinned empty on both sides.
    val readResults = if (writeError != null) buildJsonArray { } else buildJsonArray {
        for (element in (root["reads"] as? JsonArray ?: JsonArray(emptyList()))) {
            val spec = element.jsonObject
            val result = runRead(ExportSpoolReader(file), spec, index)
            add(buildJsonObject {
                if (result.records.isNotEmpty()) {
                    put("records", buildJsonArray {
                        result.records.forEach { (type, json) ->
                            add(buildJsonObject {
                                put("type", type)
                                put("json", json)
                            })
                        }
                    })
                }
                if (result.payloads.isNotEmpty()) {
                    put("payloads", buildJsonArray {
                        result.payloads.forEach { add(it) }
                    })
                }
                errorJson(result.error)?.let { put("error", it) }
                result.hasNextAfter?.let { put("hasNextAfter", it) }
                errorJson(result.errorAfter)?.let { put("errorAfter", it) }
            })
        }
    }

    val trace = buildJsonObject {
        put("bytes", bytes)
        if (writes != null) {
            if (writeError != null) {
                put("writeError", errorJson(writeError)!!)
            } else {
                val finished = index!!
                put("index", buildJsonObject {
                    put("conversations", buildJsonArray {
                        finished.conversations.forEach { conversation ->
                            add(buildJsonObject {
                                put("id", conversation.id)
                                put("dataChangedAt", conversation.dataChangedAt)
                                put("start", conversation.slice.startOffset)
                                put("end", conversation.slice.endOffset)
                            })
                        }
                    })
                    put("taskSlices", buildJsonArray {
                        finished.taskSlices.forEach { slice ->
                            add(buildJsonObject {
                                put("start", slice.startOffset)
                                put("end", slice.endOffset)
                            })
                        }
                    })
                    put("watermarks", buildJsonObject {
                        finished.watermarks.forEach { (id, at) -> put(id, at) }
                    })
                })
            }
        }
        put("reads", readResults)
    }
    file.delete()
    return Json.encodeToString(JsonObject.serializer(), trace)
}
