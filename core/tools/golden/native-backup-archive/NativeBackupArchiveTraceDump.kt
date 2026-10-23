// Golden trace driver for data/NativeBackupArchive.kt, used to pin the C++ port.
//
// The archive and its collaborators compile VERBATIM from the app tree; only the Android
// Context/Uri/ParcelFileDescriptor surface is substituted (platform/). The archives themselves
// are committed fixtures built byte-exactly by gen_scenarios.py, so both ports read the same
// bytes. The driver records one line group per op; errors are recorded with the same
// `message ?: simpleName` rule the production importers rely on. Absolute scenario paths are
// masked as `<dir>` because the C++ test runs from a different working directory.
//
// Build/run: see run_golden.sh in this directory.
package native.backup.archive.golden

import android.content.ContentResolver
import android.content.Context
import android.net.Uri
import android.os.ParcelFileDescriptor
import com.newoether.agora.data.NativeBackupArchive
import com.newoether.agora.data.readToEnd
import java.io.ByteArrayInputStream
import java.io.File
import java.io.InputStream
import java.nio.ByteBuffer
import java.nio.channels.ClosedChannelException
import java.nio.channels.NonWritableChannelException
import java.nio.channels.SeekableByteChannel
import java.nio.charset.CharacterCodingException
import java.nio.charset.CodingErrorAction
import kotlinx.serialization.SerialName
import kotlinx.serialization.Serializable
import kotlinx.serialization.json.Json

private val json = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

/** Field-safe escaping: no raw separators or control characters survive. */
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

/**
 * zip-reader's content rule: strict UTF-8 without ISO control characters renders as text,
 * everything else as `hex:<lowercase hex>`.
 */
private fun render(bytes: ByteArray): String {
    val decoded = try {
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.REPORT)
            .onUnmappableCharacter(CodingErrorAction.REPORT)
            .decode(ByteBuffer.wrap(bytes))
        true
    } catch (_: CharacterCodingException) {
        false
    }
    val controlFree = bytes.none { raw ->
        val c = raw.toInt() and 0xFF
        c < 0x20 || c in 0x7F..0x9F
    }
    return if (decoded && controlFree) {
        String(bytes, Charsets.UTF_8)
    } else {
        "hex:" + bytes.joinToString("") { "%02x".format(it) }
    }
}

@Serializable
private data class Op(
    val op: String,
    val mode: String = "",
    val name: String = "",
    val key: String = "",
    val count: Int = 0,
    val target: String = "",
    val dest: String = "",
    val conversations: Boolean = false,
    val settings: Boolean = false,
    val version: Int = 0,
    @SerialName("metadataLimit") val metadataLimit: Long? = null,
    @SerialName("maxBytes") val maxBytes: Long? = null,
    @SerialName("available") val available: Long? = null,
    @SerialName("fontLimit") val fontLimit: Long? = null,
    @SerialName("channelFixture") val channelFixture: String = "",
    @SerialName("declaredSize") val declaredSize: Long? = null,
    @SerialName("expectedCrc") val expectedCrc: Long? = null,
    @SerialName("bufferBytes") val bufferBytes: Int? = null,
    @SerialName("sourceName") val sourceName: String = "",
    @SerialName("input") val input: String = "",
)

@Serializable
private data class Scenario(
    val name: String,
    val archive: String,
    val ops: List<Op>,
    @SerialName("setupFiles") val setupFiles: List<String> = emptyList(),
)

/** Seekable byte channel over a byte array (the `open(channel)` paths). */
private class BytesChannel(private val bytes: ByteArray) : SeekableByteChannel {
    private var position = 0
    private var open = true
    private fun checkOpen() {
        if (!open) throw ClosedChannelException()
    }

    override fun read(dst: ByteBuffer): Int {
        checkOpen()
        if (position >= bytes.size) return -1
        val take = minOf(dst.remaining(), bytes.size - position)
        dst.put(bytes, position, take)
        position += take
        return take
    }

    override fun write(src: ByteBuffer): Int = throw UnsupportedOperationException()
    override fun position(): Long { checkOpen(); return position.toLong() }
    override fun position(newPosition: Long): SeekableByteChannel {
        checkOpen(); position = newPosition.toInt(); return this
    }

    override fun size(): Long { checkOpen(); return bytes.size.toLong() }
    override fun truncate(size: Long): SeekableByteChannel = throw NonWritableChannelException()
    override fun isOpen(): Boolean = open
    override fun close() { open = false }
}

/** A channel that refuses random access, like a plain pipe. */
private class NonSeekableChannel : SeekableByteChannel {
    private var open = true
    override fun read(dst: ByteBuffer): Int { return -1 }
    override fun write(src: ByteBuffer): Int = throw UnsupportedOperationException()
    override fun position(): Long = throw UnsupportedOperationException("no seek")
    override fun position(newPosition: Long): SeekableByteChannel =
        throw UnsupportedOperationException("no seek")

    override fun size(): Long = throw UnsupportedOperationException("no seek")
    override fun truncate(size: Long): SeekableByteChannel = throw NonWritableChannelException()
    override fun isOpen(): Boolean = open
    override fun close() { open = false }
}

fun main(args: Array<String>) {
    require(args.size == 2) { "usage: NativeBackupArchiveTraceDump <scenario.json> <tempDir>" }
    val scenario = json.decodeFromString(Scenario.serializer(), File(args[0]).readText())
    require(scenario.name == File(args[0]).nameWithoutExtension) {
        "scenario name '${scenario.name}' must match the file stem"
    }

    val workDir = File(args[1], "scenario_${scenario.name}").absoluteFile
    workDir.deleteRecursively()
    workDir.mkdirs()

    val fixture = File("fixtures", scenario.archive)
    val ownedCopy = File(workDir, fixture.name)
    if (fixture.isFile) fixture.copyTo(ownedCopy, overwrite = true)

    for (rel in scenario.setupFiles) {
        val file = File(workDir, rel)
        file.parentFile?.mkdirs()
        file.writeText("")
    }

    fun context(resolver: ContentResolver) = Context(resolver)

    val out = StringBuilder()
    fun mask(value: String): String = value.replace(workDir.absolutePath, "<dir>")
    fun line(vararg fields: String) {
        out.append(fields.joinToString("|") { esc(mask(it)) }).append('\n')
    }

    var archive: NativeBackupArchive? = null
    val streams = mutableMapOf<String, InputStream>()

    scenario.ops.forEachIndexed { index, step ->
        out.append("op|").append(index).append('|').append(step.op).append('\n')
        try {
            when (step.op) {
                "open" -> {
                    val limit = step.metadataLimit ?: NativeBackupArchive.MAX_METADATA_BYTES
                    val opened: NativeBackupArchive? = when (step.mode) {
                        "file" -> NativeBackupArchive.open(ownedCopy, limit)
                        "uri" -> NativeBackupArchive.open(
                            context(ContentResolver { uri ->
                                ParcelFileDescriptor(File(uri.toString().removePrefix("file://")))
                            }),
                            Uri.parse("file://${ownedCopy.absolutePath}"),
                        )
                        "uri_null" -> NativeBackupArchive.open(
                            context(ContentResolver { null }),
                            Uri.parse("file://${ownedCopy.absolutePath}"),
                        )
                        "channel" -> {
                            val source = if (step.channelFixture.isNotEmpty()) {
                                File("fixtures", step.channelFixture).readBytes()
                            } else {
                                ownedCopy.readBytes()
                            }
                            NativeBackupArchive.open(BytesChannel(source), limit)
                        }
                        "nonseek" -> NativeBackupArchive.open(NonSeekableChannel(), limit)
                        else -> error("unknown open mode: ${step.mode}")
                    }
                    archive = opened
                    line("open", opened?.let { "ok" } ?: "null")
                }
                "names" -> {
                    val names = requireArchive(archive).names()
                    line("names", names.size.toString())
                    names.forEach { name -> line("entry", name) }
                }
                "size" -> line("size", requireArchive(archive).size(step.name).toString())
                "has" -> line("has", requireArchive(archive).has(step.name).toString())
                "bytes" -> {
                    val value = requireArchive(archive).bytes(step.name)
                    if (value == null) line("bytes", "null") else line("bytes", value.size.toString(), render(value))
                }
                "prefix" -> {
                    val value = requireArchive(archive).prefix(step.name, step.count)
                    if (value == null) line("prefix", "null") else line("prefix", value.size.toString(), render(value))
                }
                "stream_open" -> {
                    val value = requireArchive(archive).stream(step.name)
                    if (value == null) {
                        line("stream", step.key, "missing")
                    } else {
                        streams[step.key] = value
                        line("stream", step.key, "opened")
                    }
                }
                "stream_read" -> {
                    val stream = streams[step.key] ?: error("Stream not open: ${step.key}")
                    val buffer = ByteArray(step.count)
                    val read = stream.read(buffer, 0, step.count)
                    if (read < 0) {
                        line("read", "-1")
                    } else {
                        line("read", read.toString(), render(buffer.copyOf(read)))
                    }
                }
                "stream_skip" -> {
                    val stream = streams[step.key] ?: error("Stream not open: ${step.key}")
                    line("skip", stream.skip(step.count.toLong()).toString())
                }
                "stream_read_to_end" -> {
                    val stream = streams[step.key] ?: error("Stream not open: ${step.key}")
                    stream.readToEnd()
                    line("end", "ok")
                }
                "stream_close" -> {
                    val stream = streams[step.key] ?: error("Stream not open: ${step.key}")
                    stream.close()
                    line("close", step.key, "ok")
                }
                "copy_to" -> {
                    val target = File(workDir, step.target)
                    try {
                        val copied = requireArchive(archive).copyTo(
                            step.name,
                            target,
                            step.maxBytes ?: Long.MAX_VALUE,
                        ) { step.available ?: Long.MAX_VALUE }
                        if (copied == null) {
                            line("copy", "null")
                            line("exists", "false")
                        } else {
                            line("copy", copied.toString(), render(target.readBytes()))
                        }
                    } catch (failure: Throwable) {
                        line("error", failure.message ?: failure::class.simpleName ?: "error")
                        line("exists", "false")
                    }
                }
                "copy_stream_raw" -> {
                    val target = File(workDir, step.target)
                    try {
                        val copied = NativeBackupArchive.copyStreamToFile(
                            input = ByteArrayInputStream(step.input.toByteArray()),
                            target = target,
                            declaredSize = step.declaredSize ?: -1L,
                            expectedCrc = step.expectedCrc ?: -1L,
                            maxBytes = step.maxBytes ?: Long.MAX_VALUE,
                            availableBytes = { step.available ?: Long.MAX_VALUE },
                            bufferBytes = step.bufferBytes ?: 32 * 1024,
                            sourceName = step.sourceName.ifEmpty { target.name },
                        )
                        line("copy", copied.toString(), render(target.readBytes()))
                    } catch (failure: Throwable) {
                        line("error", failure.message ?: failure::class.simpleName ?: "error")
                        line("exists", "false")
                    }
                }
                "preflight" -> {
                    val required = requireArchive(archive).preflightImportResources(
                        conversationsSelected = step.conversations,
                        settingsSelected = step.settings,
                        archiveVersion = step.version,
                        destinationRoot = File(workDir, step.dest),
                        customFontLimitBytes = step.fontLimit ?: Long.MAX_VALUE,
                        availableBytes = { step.available ?: Long.MAX_VALUE },
                    )
                    line("preflight", required.toString())
                }
                "temp_exists" -> line("exists", ownedCopy.exists().toString())
                "close" -> {
                    requireArchive(archive).close()
                    archive = null
                    line("close", "ok")
                }
                else -> error("unknown op: ${step.op}")
            }
        } catch (failure: Throwable) {
            line("error", failure.message ?: failure::class.simpleName ?: "error")
        }
    }
    print(out)
}

private fun requireArchive(archive: NativeBackupArchive?): NativeBackupArchive =
    archive ?: error("Archive is not open")
