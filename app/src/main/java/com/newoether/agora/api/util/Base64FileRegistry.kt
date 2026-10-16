package com.newoether.agora.api.util

import java.io.File
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
)

/**
 * Projects image files into JSON as opaque placeholders. The final request body replaces each
 * placeholder with base64 while the request is written, so image bytes never become a String or a
 * whole-request byte array.
 */
internal class Base64FileRegistry {
    private val files = mutableListOf<Base64FilePart>()

    fun register(path: String): String? {
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
            )
        }.getOrNull() ?: return null
        files += part
        return part.placeholder
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
