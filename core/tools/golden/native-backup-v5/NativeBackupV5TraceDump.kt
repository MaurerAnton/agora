// Golden trace driver for data/NativeBackupV5Writer.kt and data/NativeBackupV5Baseline.kt.
//
// Both V5 files compile verbatim (the driver shares their module, so `internal` needs no
// widening). The platform boundary is Apache Commons Compress: the writer half produces archives
// with ZipArchiveOutputStream and every step reads back with ZipFile, so the traces pin the
// archive at the entry level — name, content, CRC, size and method — which is exactly the surface
// the already-pinned core zip_reader and the importers consume. The compressed byte stream itself
// belongs to the platform compressor and is deliberately not recorded.
//
// The one byte-level operation here is corruptEntry: it flips the stored CRC field of a named
// entry (local header and central directory) so the baseline integrity check has real damage to
// reject. Damaged archives are created with "dump": false — their bytes are a test fixture, and
// what the module does with them (canReuse == false) is what the trace pins.
package native.backup.v5.golden

import com.newoether.agora.data.NativeBackupV5Baseline
import com.newoether.agora.data.NativeBackupV5Writer
import java.io.File
import org.apache.commons.compress.archivers.zip.ZipArchiveEntry
import org.apache.commons.compress.archivers.zip.ZipArchiveOutputStream
import org.apache.commons.compress.archivers.zip.ZipFile
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

private val json = Json

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

/** Mutable driver state for one scenario run. */
private class Scenario {
    val workspace = File("v5_workspace")
    val outputs = mutableMapOf<String, ZipArchiveOutputStream>()
    val dumpFiles = mutableMapOf<String, Boolean>()
    val copiedMedia = mutableMapOf<String, MutableSet<String>>()

    fun reset() {
        workspace.deleteRecursively()
        workspace.mkdirs()
    }

    fun file(key: String): File = File(workspace, key)

    fun outputStream(key: String): ZipArchiveOutputStream =
        outputs.getOrPut(key) {
            dumpFiles.putIfAbsent(key, true)
            ZipArchiveOutputStream(file(key))
        }

    fun closeOutputs() {
        outputs.values.forEach { it.close() }
        outputs.clear()
    }
}

private fun render(scenarioFile: File): String {
    val root = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val scenario = Scenario()
    scenario.reset()
    try {
        val steps = buildJsonArray {
            val stepList = root["steps"] as? JsonArray ?: JsonArray(emptyList())
            for (element in stepList) {
                val step = element.jsonObject
                val op = step.string("op")
                val result = try {
                    executeOperation(op, step, scenario)
                } catch (failure: Throwable) {
                    buildJsonObject {
                        put("error", failure::class.simpleName ?: failure::class.java.name)
                        put("message", failure.message ?: "")
                    }
                }
                add(buildJsonObject { put("op", op); put("result", result) })
            }
        }
        scenario.closeOutputs()
        val archives = buildJsonObject {
            for (key in scenario.dumpFiles.keys.sorted()) {
                if (scenario.dumpFiles[key] == false) continue
                put(key, dumpArchive(scenario.file(key)))
            }
        }
        val trace = buildJsonObject {
            put("steps", steps)
            put("archives", archives)
        }
        return json.encodeToString(JsonObject.serializer(), trace)
    } finally {
        scenario.closeOutputs()
        scenario.workspace.deleteRecursively()
    }
}

// ---- operations ----------------------------------------------------------------------------

private fun executeOperation(op: String, step: JsonObject, scenario: Scenario): JsonElement {
    return when (op) {
        "createBaseline" -> createBaseline(step, scenario)
        "openOrNull" -> {
            val baseline = NativeBackupV5Baseline.openOrNull(scenario.file(step.string("file")))
            if (baseline == null) {
                JsonNull
            } else {
                baseline.use {
                    buildJsonObject {
                        put("index", buildJsonObject {
                            put("conversations", buildJsonArray {
                                it.index.conversations.forEach { item -> add(entryJson(item)) }
                            })
                        })
                    }
                }
            }
        }
        "canReuse" -> {
            val baseline = NativeBackupV5Baseline.openOrNull(scenario.file(step.string("file")))
            if (baseline == null) {
                JsonNull
            } else {
                baseline.use {
                    JsonPrimitive(
                        it.canReuse(step.string("id"), step.long("dataChangedAt")),
                    )
                }
            }
        }
        "indexEntry" -> {
            val baseline = NativeBackupV5Baseline.openOrNull(scenario.file(step.string("file")))
            if (baseline == null) {
                JsonNull
            } else {
                baseline.use {
                    it.indexEntry(step.string("id"))?.let(::entryJson) ?: JsonNull
                }
            }
        }
        "writeConversationEntry" -> {
            val zip = scenario.outputStream(step.string("file"))
            val entry = NativeBackupV5Writer.writeConversationEntry(
                zip = zip,
                conversationId = step.string("conversationId"),
                dataChangedAt = step.long("dataChangedAt"),
                conversationJson = step.string("conversation"),
                runs = step.stringArray("runs").iterator(),
                messages = step.stringArray("messages").iterator(),
                loops = step.stringArray("loops").iterator(),
                mediaEntries = step.stringArray("mediaEntries").toSet(),
            )
            entryJson(entry)
        }
        "writeTasksEntry" -> {
            val zip = scenario.outputStream(step.string("file"))
            NativeBackupV5Writer.writeTasksEntry(zip, step.stringArray("tasks").iterator())
            JsonNull
        }
        "writeConversationIndex" -> {
            val zip = scenario.outputStream(step.string("file"))
            val entries = step["entries"]!!.jsonArray.map { entryOf(it.jsonObject) }
            NativeBackupV5Writer.writeConversationIndex(zip, entries)
            JsonNull
        }
        "copyConversationFromBaseline" -> {
            val file = step.string("file")
            val zip = scenario.outputStream(file)
            val baseline = NativeBackupV5Baseline.openOrNull(
                scenario.file(step.string("baseline")),
            ) ?: return JsonNull
            val copiedMedia = scenario.copiedMedia.getOrPut(file) { mutableSetOf() }
            step["copiedMedia"]?.jsonArray?.forEach { copiedMedia.add(it.jsonPrimitive.content) }
            val entry = NativeBackupV5Writer.copyConversationFromBaseline(
                zip = zip,
                baseline = baseline,
                baselineEntry = entryOf(step["entry"]!!.jsonObject),
                copiedMedia = copiedMedia,
            )
            baseline.close()
            buildJsonObject {
                put("entry", entryJson(entry))
                put("copiedMedia", buildJsonArray { copiedMedia.forEach { add(JsonPrimitive(it)) } })
            }
        }
        "copyRaw" -> {
            val baseline = NativeBackupV5Baseline.openOrNull(
                scenario.file(step.string("baseline")),
            ) ?: return JsonNull
            baseline.use {
                it.copyRaw(step.string("name"), scenario.outputStream(step.string("file")))
            }
            JsonNull
        }
        "corruptEntry" -> {
            val file = scenario.file(step.string("file"))
            scenario.dumpFiles[file.name] = false
            corruptEntryCrc(file, step.string("name"))
            JsonNull
        }
        else -> error("unknown operation $op")
    }
}

private fun createBaseline(step: JsonObject, scenario: Scenario): JsonElement {
    val key = step.string("file")
    scenario.dumpFiles[key] = step.optionalBoolean("dump") ?: true
    val zip = ZipArchiveOutputStream(scenario.file(key))

    // Manifest: the baseline reader only checks version, incremental revision and categories.
    val manifestText = step.optionalString("manifestText")
        ?: json.encodeToString(JsonObject.serializer(), step["manifest"]!!.jsonObject)
    zip.putArchiveEntry(ZipArchiveEntry("manifest.json"))
    zip.write(manifestText.encodeToByteArray())
    zip.closeArchiveEntry()

    // Media first, then conversations, then the index — the write order the port mirrors.
    for (element in step["media"] as? JsonArray ?: JsonArray(emptyList())) {
        val row = element.jsonObject
        zip.putArchiveEntry(ZipArchiveEntry(row.string("name")))
        zip.write(row.string("content").encodeToByteArray())
        zip.closeArchiveEntry()
    }

    val derived = buildJsonArray {
        for (element in step["items"] as? JsonArray ?: JsonArray(emptyList())) {
            val item = element.jsonObject
            val entry = NativeBackupV5Writer.writeConversationEntry(
                zip = zip,
                conversationId = item.string("id"),
                dataChangedAt = item.long("dataChangedAt"),
                conversationJson = item.string("conversation"),
                runs = item.stringArray("runs").iterator(),
                messages = item.stringArray("messages").iterator(),
                loops = item.stringArray("loops").iterator(),
                mediaEntries = item.stringArray("mediaEntries").toSet(),
            )
            add(entryJson(entry))
        }
    }

    val indexText = step.optionalString("indexText")
        ?: json.encodeToString(
            com.newoether.agora.data.NativeConversationIndex.serializer(),
            com.newoether.agora.data.NativeConversationIndex(
                (step["index"] as? JsonArray ?: derived).map { entryOf(it.jsonObject) },
            ),
        )
    zip.putArchiveEntry(ZipArchiveEntry("conv/index.json"))
    zip.write(indexText.encodeToByteArray())
    zip.closeArchiveEntry()
    zip.close()
    return JsonNull
}

/** Flips the stored CRC of a named entry in both the local header and the central directory. */
private fun corruptEntryCrc(file: File, name: String) {
    val bytes = file.readBytes()
    var offset = 0
    while (offset + 4 <= bytes.size) {
        if (bytes.u32(offset) == 0x04034B50L) {           // local file header
            val nameLength = bytes.u16(offset + 26)
            if (bytes.nameAt(offset + 30, nameLength) == name) {
                bytes.patchU32(offset + 14)
            }
        }
        if (bytes.u32(offset) == 0x02014B50L) {           // central directory header
            val nameLength = bytes.u16(offset + 28)
            if (bytes.nameAt(offset + 46, nameLength) == name) {
                bytes.patchU32(offset + 16)
            }
        }
        offset++
    }
    file.writeBytes(bytes)
}

private fun ByteArray.u32(at: Int): Long =
    (this[at].toLong() and 0xff) or
        ((this[at + 1].toLong() and 0xff) shl 8) or
        ((this[at + 2].toLong() and 0xff) shl 16) or
        ((this[at + 3].toLong() and 0xff) shl 24)

private fun ByteArray.u16(at: Int): Int =
    (this[at].toInt() and 0xff) or ((this[at + 1].toInt() and 0xff) shl 8)

private fun ByteArray.nameAt(at: Int, length: Int): String =
    if (at + length <= size) String(this, at, length, Charsets.UTF_8) else ""

private fun ByteArray.patchU32(at: Int) {
    val flipped = u32(at) xor 0xFFFFFFFFL
    this[at] = (flipped and 0xFF).toByte()
    this[at + 1] = ((flipped shr 8) and 0xFF).toByte()
    this[at + 2] = ((flipped shr 16) and 0xFF).toByte()
    this[at + 3] = ((flipped shr 24) and 0xFF).toByte()
}

// ---- archive dump -------------------------------------------------------------------------

private fun dumpArchive(file: File): JsonElement {
    if (!file.isFile) return JsonArray(emptyList())
    return buildJsonArray {
        ZipFile.builder().setFile(file).get().use { zip ->
            zip.entries.toList().forEach { entry ->
                val content = zip.getInputStream(entry).use { it.readBytes() }
                add(
                    buildJsonObject {
                        put("name", JsonPrimitive(entry.name))
                        put("content", JsonPrimitive(String(content, Charsets.UTF_8)))
                        put("crc", JsonPrimitive(entry.crc))
                        put("size", JsonPrimitive(entry.size))
                        put("method", JsonPrimitive(entry.method.toInt()))
                    },
                )
            }
        }
    }
}

// ---- scenario entities --------------------------------------------------------------------

private fun entryJson(entry: com.newoether.agora.data.NativeConversationIndexEntry): JsonElement =
    buildJsonObject {
        put("id", JsonPrimitive(entry.id))
        put("dataChangedAt", JsonPrimitive(entry.dataChangedAt))
        put("entry", JsonPrimitive(entry.entry))
        put("mediaEntries", buildJsonArray {
            entry.mediaEntries.forEach { add(JsonPrimitive(it)) }
        })
    }

private fun entryOf(source: JsonObject): com.newoether.agora.data.NativeConversationIndexEntry =
    com.newoether.agora.data.NativeConversationIndexEntry(
        id = source.string("id"),
        dataChangedAt = source.long("dataChangedAt"),
        entry = source.string("entry"),
        mediaEntries = source.stringArray("mediaEntries"),
    )

// ---- JSON helpers -------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.long(name: String): Long = string(name).toLong()

private fun JsonObject.optionalBoolean(name: String): Boolean? =
    optionalString(name)?.toBooleanStrictOrNull()

private fun JsonObject.stringArray(name: String): List<String> =
    (this[name] as? JsonArray)?.map { it.jsonPrimitive.content } ?: emptyList()
