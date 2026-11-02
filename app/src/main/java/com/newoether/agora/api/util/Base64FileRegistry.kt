package com.newoether.agora.api.util

import com.newoether.agora.model.ImageArtifact
import okhttp3.RequestBody
import java.io.File
import java.io.IOException
import java.security.MessageDigest
import java.util.UUID

/**
 * One immutable file projected into the request as a placeholder. [sha256] pins the exact bytes:
 * the streaming body re-verifies them on every write, because a request may be replayed for
 * endpoint fallback or transport retries.
 */
internal class Base64FilePart(
    val placeholder: String,
    val file: File,
    val byteCount: Long,
    val lastModified: Long,
    val sha256: ByteArray,
    /**
     * What the envelope labels the payload with: the mime sniffed from the file's current bytes,
     * or the artifact's declaration when the signature is unrecognized; empty when only a path was
     * registered.
     */
    val mimeType: String = "",
) {
    val diagnosticMarker: String
        get() = "[STREAMED_BASE64:${byteCount}_BYTES]"

    fun metadataMatches(): Boolean =
        file.isFile && file.length() == byteCount && file.lastModified() == lastModified
}

/**
 * The template JSON split around every registered placeholder, plus the exact length of the final
 * wire body. [segments] always has one more entry than [parts]: writing the body emits
 * segments[0], base64(parts[0]), segments[1], ... in order. The okhttp form of this descriptor is
 * `StreamingJsonBody.toRequestBody()` in StreamingJsonRequestBody.kt.
 */
internal data class StreamingJsonBody(
    val segments: List<ByteArray>,
    val parts: List<Base64FilePart>,
    val diagnosticJson: String,
    val contentLength: Long,
) {
    /**
     * The okhttp form of this descriptor. Callers that build the request envelope read it as a
     * property; [toRequestBody] stays the spelling used inside the providers.
     */
    val body: RequestBody
        get() = toRequestBody()
}

/**
 * Projects image files into JSON as opaque placeholders. The final request body replaces each
 * placeholder with base64 while the request is written, so image bytes never become a String or a
 * whole-request byte array.
 */
internal class Base64FileRegistry {
    private val files = mutableListOf<Base64FilePart>()

    fun register(path: String): String? = registerPart(path, "")?.placeholder

    /**
     * Projects an artifact onto the wire. What the envelope declares is the mime of the bytes that
     * are really on disk, sniffed from their signature: a declaration measured earlier goes stale
     * the moment the file is rewritten and would mislabel the payload the endpoint decodes.
     * Sniffing stays deliberately shallow and returns null for anything it cannot name with
     * confidence, so payloads without a known signature (including the minimal byte-parity
     * fixtures) keep the artifact's own declaration.
     */
    fun register(artifact: ImageArtifact): Base64FilePart? =
        registerPart(artifact.path, measuredImageMimeType(artifact.path) ?: artifact.mimeType)

    private fun measuredImageMimeType(path: String): String? = try {
        File(path).inputStream().use { input ->
            val head = ByteArray(12)
            var read = 0
            while (read < head.size) {
                val count = input.read(head, read, head.size - read)
                if (count < 0) break
                read += count
            }
            sniffImageMimeType(head, read)
        }
    } catch (_: IOException) {
        null
    }

    private fun sniffImageMimeType(head: ByteArray, length: Int): String? {
        fun matches(vararg expected: Int): Boolean =
            length >= expected.size && expected.withIndex().all { (index, byte) ->
                head[index] == byte.toByte()
            }
        return when {
            matches(0x89, 0x50, 0x4E, 0x47, 0x0D, 0x0A, 0x1A, 0x0A) -> "image/png"
            matches(0xFF, 0xD8, 0xFF) -> "image/jpeg"
            matches(0x52, 0x49, 0x46, 0x46) && length >= 12 &&
                head.copyOfRange(8, 12).contentEquals("WEBP".toByteArray()) -> "image/webp"
            matches(0x47, 0x49, 0x46, 0x38) -> "image/gif"
            matches(0x42, 0x4D) -> "image/bmp"
            else -> null
        }
    }

    private fun registerPart(path: String, mimeType: String): Base64FilePart? {
        val part = runCatching {
            val file = File(path).canonicalFile
            if (!file.isFile) return@runCatching null
            val byteCount = file.length()
            val lastModified = file.lastModified()
            val sha256 = calculateSha256(file)
            if (!file.isFile || file.length() != byteCount || file.lastModified() != lastModified) {
                return@runCatching null
            }
            Base64FilePart(
                placeholder = "__AGORA_BASE64_${UUID.randomUUID()}__",
                file = file,
                byteCount = byteCount,
                lastModified = lastModified,
                sha256 = sha256,
                mimeType = mimeType,
            )
        }.getOrNull() ?: return null
        files += part
        return part
    }

    fun prepare(templateJson: String): StreamingJsonBody? {
        if (files.isEmpty()) return null
        val positionedFiles = files.map { part ->
            val position = templateJson.indexOf(part.placeholder)
            require(position >= 0) { "Base64 placeholder is missing from serialized request" }
            require(templateJson.indexOf(part.placeholder, position + part.placeholder.length) < 0) {
                "Base64 placeholder occurs more than once in serialized request"
            }
            position to part
        }.sortedBy { it.first }

        val segments = ArrayList<ByteArray>(positionedFiles.size + 1)
        val orderedFiles = ArrayList<Base64FilePart>(positionedFiles.size)
        val diagnosticJson = StringBuilder(templateJson.length)
        var cursor = 0
        for ((position, part) in positionedFiles) {
            require(position >= cursor) { "Base64 placeholders overlap" }
            val segment = templateJson.substring(cursor, position)
            segments += segment.toByteArray(Charsets.UTF_8)
            orderedFiles += part
            diagnosticJson.append(segment).append(part.diagnosticMarker)
            cursor = position + part.placeholder.length
        }
        val finalSegment = templateJson.substring(cursor)
        segments += finalSegment.toByteArray(Charsets.UTF_8)
        diagnosticJson.append(finalSegment)

        return StreamingJsonBody(
            segments = segments,
            parts = orderedFiles,
            diagnosticJson = diagnosticJson.toString(),
            contentLength = calculateContentLength(segments, orderedFiles),
        )
    }

    private companion object {
        fun calculateSha256(file: File): ByteArray {
            val digest = MessageDigest.getInstance("SHA-256")
            file.inputStream().use { input ->
                val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                while (true) {
                    val read = input.read(buffer)
                    if (read < 0) break
                    digest.update(buffer, 0, read)
                }
            }
            return digest.digest()
        }

        fun calculateContentLength(
            segments: List<ByteArray>,
            files: List<Base64FilePart>,
        ): Long {
            var total = 0L
            segments.forEach { segment -> total = Math.addExact(total, segment.size.toLong()) }
            files.forEach { part ->
                require(part.byteCount >= 0L) { "Image attachment length is invalid" }
                val completeGroups = part.byteCount / 3L
                val remainderLength = if (part.byteCount % 3L == 0L) 0L else 4L
                val encodedLength = Math.addExact(
                    Math.multiplyExact(completeGroups, 4L),
                    remainderLength,
                )
                total = Math.addExact(total, encodedLength)
            }
            return total
        }
    }
}
