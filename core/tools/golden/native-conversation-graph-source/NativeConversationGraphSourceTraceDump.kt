// Golden trace driver for data/NativeConversationGraphSource.kt.
//
// NativeConversationGraphSource.kt compiles VERBATIM from the app tree together with
// NativeBackupFormat.kt (for the index decode) and the four-line `InputStream.readToEnd()`
// lifted by this stand's extractor. The platform substitute this stand pins is
// android.util.JsonReader/JsonWriter/JsonToken + MalformedJsonException and the internal
// StringPool, vendored verbatim from AOSP (vendor/) and compiled by javac, so the streaming
// parse, the line/column arithmetic, the lenient rules and the Double.toString emission are
// Android's, not a reimplementation.
//
// The entry surface (NativeGraphEntrySource) is substituted by an in-memory/file-backed double:
// bytes()/has() serve the scenario's entries, stream() hands out a FileInputStream over the
// same bytes — optionally wrapped so that reaching the end throws instead of reporting EOF,
// which is what a checked archive stream does at a size/CRC mismatch and what the source's
// parse-then-drain ordering must propagate.
//
// Each step reports the chained graph content (or the thrown error), what a read after close()
// does on the handed-out stream, and the cache directory's remaining children (the spool must
// be gone on every path).
package native.conversation.graph.source.golden

import com.newoether.agora.data.NativeConversationGraphSource
import com.newoether.agora.data.NativeGraphEntrySource
import java.io.File
import java.io.FileInputStream
import java.io.IOException
import java.io.InputStream
import java.nio.file.Files
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.intOrNull
import kotlinx.serialization.json.put

private val scenarioJson = Json { ignoreUnknownKeys = true }

/** An entry whose stream throws at its end instead of reporting EOF (checked-archive semantics). */
private class FaultingStream(private val delegate: InputStream, private val message: String) :
    InputStream() {
    override fun read(): Int {
        val value = delegate.read()
        if (value == -1) throw IOException(message)
        return value
    }

    override fun read(b: ByteArray, off: Int, len: Int): Int {
        val count = delegate.read(b, off, len)
        if (count == -1) throw IOException(message)
        return count
    }

    override fun close() = delegate.close()
}

private class MapEntrySource(
    private val entries: Map<String, ByteArray>,
    private val files: Map<String, File>,
    private val streamNulls: Set<String>,
    private val drainFaults: Map<String, String>,
) : NativeGraphEntrySource {
    override fun has(name: String): Boolean = entries.containsKey(name)

    override fun bytes(name: String): ByteArray? = entries[name]

    override fun stream(name: String): InputStream? {
        if (!entries.containsKey(name) || name in streamNulls) return null
        val delegate = FileInputStream(files.getValue(name))
        val fault = drainFaults[name] ?: return delegate
        return FaultingStream(delegate, fault)
    }
}

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
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

private fun readAfterCloseLabel(stream: InputStream): String = try {
    val value = stream.read()
    if (value == -1) "eof" else "data:$value"
} catch (failure: Throwable) {
    "${failure::class.simpleName}: ${failure.message}"
}

private fun render(scenarioFile: File): String {
    val root = Files.createTempDirectory("agora-graph-source-").toFile()
    try {
        val scenario = scenarioJson.parseToJsonElement(scenarioFile.readText()).jsonObject
        val cacheDir = File(root, "cache").also { it.mkdirs() }
        val entryDir = File(root, "entries").also { it.mkdirs() }

        val entries = linkedMapOf<String, ByteArray>()
        val files = linkedMapOf<String, File>()
        // entries is an object: name -> content (UTF-8); the file backs stream().
        val entriesObject = scenario["entries"]?.jsonObject ?: JsonObject(emptyMap())
        for ((name, content) in entriesObject) {
            entries[name] = content.jsonPrimitive.content.toByteArray(Charsets.UTF_8)
            val file = File(entryDir, name.replace('/', '_'))
            file.writeBytes(entries[name]!!)
            files[name] = file
        }
        val streamNulls = (scenario["streamNull"]?.jsonArray ?: JsonArray(emptyList()))
            .map { it.jsonPrimitive.content }
            .toSet()
        val drainFaults = linkedMapOf<String, String>()
        scenario["drainFaults"]?.jsonObject?.forEach { (name, message) ->
            drainFaults[name] = message.jsonPrimitive.content
        }
        val version = scenario["version"]?.jsonPrimitive?.intOrNull ?: 5

        val steps = scenario["steps"]?.jsonArray ?: JsonArray(emptyList())
        val rendered = mutableListOf<JsonElement>()
        for (element in steps) {
            val step = element.jsonObject
            val op = step["op"]?.jsonPrimitive?.content ?: "open"

            var source: NativeConversationGraphSource? = null
            var closedByOp = false
            val result = try {
                val archive = MapEntrySource(entries, files, streamNulls, drainFaults)
                val opened = NativeConversationGraphSource.open(archive, version, cacheDir)
                source = opened
                val stream = opened.open()
                when (op) {
                    "open" -> {
                        val content = stream.readBytes().toString(Charsets.UTF_8)
                        opened.close()
                        closedByOp = true
                        buildJsonObject {
                            put("content", content)
                            put("readAfterClose", readAfterCloseLabel(stream))
                        }
                    }
                    "open-partial" -> {
                        val firstByte = stream.read()
                        opened.close()
                        closedByOp = true
                        buildJsonObject {
                            put("firstByte", firstByte)
                            put("readAfterClose", readAfterCloseLabel(stream))
                        }
                    }
                    else -> error("unknown step op $op")
                }
            } catch (failure: Throwable) {
                buildJsonObject {
                    put("error", buildJsonObject {
                        put("type", failure::class.simpleName ?: failure::class.java.name)
                        put("message", failure.message ?: "")
                    })
                }
            } finally {
                if (!closedByOp) {
                    // The caller's use{} semantics: a read failure still closes the source,
                    // which closes the handed-out stream and deletes the spool.
                    runCatching { source?.close() }
                }
            }

            val cache = buildJsonArray {
                (cacheDir.listFiles() ?: emptyArray())
                    .map { it.name }
                    .sorted()
                    .forEach { add(it) }
            }
            rendered.add(buildJsonObject {
                put("op", op)
                put("result", result)
                put("cache", cache)
            })
        }

        return scenarioJson.encodeToString(
            JsonObject.serializer(),
            buildJsonObject { put("steps", JsonArray(rendered)) },
        )
    } finally {
        root.deleteRecursively()
    }
}
