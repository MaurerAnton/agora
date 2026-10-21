// Golden trace driver for the ZIP reader (the java.util.zip.ZipInputStream surface the app uses to
// read a vendor export archive).
//
// A scenario either describes entries (the driver builds the archive with java.util.zip's
// ZipOutputStream, writes the bytes to fixtures/<stem>.zip and reads them back with
// ZipInputStream) or carries raw bytes as hex (corruption cases). The trace lists every entry with
// its method, sizes, CRC and content, or the exception the JDK raised.
package zip.reader.golden

import java.io.ByteArrayInputStream
import java.io.ByteArrayOutputStream
import java.io.File
import java.nio.charset.CodingErrorAction
import java.nio.charset.StandardCharsets
import java.util.zip.CRC32
import java.util.zip.ZipEntry
import java.util.zip.ZipException
import java.util.zip.ZipInputStream
import java.util.zip.ZipOutputStream
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put

private val json = Json

/** A fixed DOS timestamp keeps the built archive byte-identical between runs. */
private const val FIXED_TIME = 1_000_000_000_000L

private fun hex(bytes: ByteArray): String {
    val out = StringBuilder(bytes.size * 2)
    for (byte in bytes) {
        val value = byte.toInt() and 0xFF
        out.append("0123456789abcdef"[value shr 4])
        out.append("0123456789abcdef"[value and 0x0F])
    }
    return out.toString()
}

private fun decodeHex(text: String): ByteArray {
    val out = ByteArray(text.length / 2)
    for (index in out.indices) {
        out[index] = text.substring(index * 2, index * 2 + 2).toInt(16).toByte()
    }
    return out
}

/** Strict UTF-8 text when possible, hex otherwise (the C++ side applies the same rule). */
private fun renderContent(bytes: ByteArray): String {
    val decoder = StandardCharsets.UTF_8.newDecoder()
        .onMalformedInput(CodingErrorAction.REPORT)
        .onUnmappableCharacter(CodingErrorAction.REPORT)
    val text = try {
        decoder.decode(java.nio.ByteBuffer.wrap(bytes)).toString()
    } catch (_: Exception) {
        return "hex:" + hex(bytes)
    }
    val hasControl = text.any { it.isISOControl() && it != '\n' && it != '\r' && it != '\t' }
    return if (hasControl) "hex:" + hex(bytes) else text
}

private fun buildArchive(entries: JsonArray): ByteArray {
    val out = ByteArrayOutputStream()
    ZipOutputStream(out).use { stream ->
        for (element in entries) {
            val spec = element.jsonObject
            val name = (spec["name"] as JsonPrimitive).content
            val content = (spec["content"] as? JsonPrimitive)?.content.orEmpty().toByteArray()
            val method = (spec["method"] as? JsonPrimitive)?.content ?: "deflate"
            val entry = ZipEntry(name)
            entry.time = FIXED_TIME
            (spec["extra"] as? JsonPrimitive)?.content?.let { entry.extra = it.toByteArray() }
            when (method) {
                "stored" -> {
                    entry.method = ZipEntry.STORED
                    val crc = CRC32()
                    crc.update(content)
                    entry.size = content.size.toLong()
                    entry.compressedSize = content.size.toLong()
                    entry.crc = crc.value
                }
                "deflate" -> entry.method = ZipEntry.DEFLATED
                else -> error("unknown method `$method`")
            }
            stream.putNextEntry(entry)
            stream.write(content)
            stream.closeEntry()
        }
    }
    return out.toByteArray()
}

private fun dumpEntry(entry: ZipEntry, content: ByteArray): JsonObject = buildJsonObject {
    put("name", entry.name)
    put("method", entry.method)
    put("size", entry.size)
    put("compressedSize", entry.compressedSize)
    put("crc", entry.crc.toLong() and 0xFFFFFFFFL)
    put("content", renderContent(content))
}

private fun readArchive(bytes: ByteArray): JsonObject {
    val entries = buildJsonArray {
        ZipInputStream(ByteArrayInputStream(bytes)).use { stream ->
            while (true) {
                val entry = stream.nextEntry ?: break
                val content = stream.readBytes()
                add(dumpEntry(entry, content))
            }
        }
    }
    return buildJsonObject { put("entries", entries) }
}

private fun readArchiveCatching(bytes: ByteArray): JsonObject = try {
    readArchive(bytes)
} catch (error: Throwable) {
    buildJsonObject {
        put("error", buildJsonObject {
            put("type", error::class.simpleName ?: error.javaClass.name)
            put("message", error.message ?: "")
        })
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
    val bytes: ByteArray = when {
        root["entries"] != null -> {
            val archive = buildArchive(root["entries"] as JsonArray)
            val fixtures = File(scenarioFile.parentFile.parentFile, "fixtures")
            fixtures.mkdirs()
            File(fixtures, scenarioFile.nameWithoutExtension + ".zip").writeBytes(archive)
            archive
        }
        else -> decodeHex((root["raw"] as JsonPrimitive).content)
    }
    val trace = readArchiveCatching(bytes)
    return Json.encodeToString(JsonObject.serializer(), trace)
}
