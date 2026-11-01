package com.newoether.agora.api.util

import android.graphics.BitmapFactory
import com.newoether.agora.api.HttpClient
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.ImageArtifact
import com.newoether.agora.util.AttachmentFiles
import com.newoether.agora.util.ImageArtifactProcessor
import kotlinx.coroutines.CancellationException
import okhttp3.MediaType
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody
import okio.BufferedSink
import java.io.File
import java.io.IOException
import java.io.InterruptedIOException
import java.io.OutputStream
import java.net.URI
import java.security.MessageDigest
import java.util.Base64

private val JSON_MEDIA_TYPE: MediaType = "application/json; charset=utf-8".toMediaType()

internal data class StreamingJsonRequest(
    val body: RequestBody,
    val diagnosticJson: String,
)

/** Occurrences retain their own availability even when several refer to the same path. */
internal fun ChatMessage.imageArtifactsForSend(): List<ImageArtifact> =
    images.zip(imageArtifactOccurrences()).mapNotNull { (reference, artifact) ->
        artifact?.takeIf { reference.isNotBlank() && !it.unavailable }
    }

internal fun readableImageArtifactFile(artifact: ImageArtifact): File? {
    val file = imageArtifactFile(artifact) ?: return null
    return file.takeIf { readImageSnapshot(it) != null }
}

private fun imageArtifactFile(artifact: ImageArtifact): File? {
    if (artifact.unavailable || artifact.path.isBlank()) return null
    return try {
        val source = if (artifact.path.startsWith("file:", ignoreCase = true)) {
            File(URI(artifact.path))
        } else {
            File(artifact.path)
        }
        if (source.isAbsolute) source.canonicalFile.takeIf { it.isFile && it.canRead() } else null
    } catch (_: IOException) {
        null
    } catch (_: IllegalArgumentException) {
        null
    } catch (_: SecurityException) {
        null
    }
}

private data class ImageSnapshot(val bytes: ByteArray, val mimeType: String)

/** A single bounded original-byte snapshot is decoded before any of its JSON is written. */
private fun readImageSnapshot(file: File): ImageSnapshot? = try {
    checkImageReadCancellation()
    val length = file.length()
    if (!file.isFile || !file.canRead() || length !in 1..AttachmentFiles.MAX_ATTACHMENT_BYTES) {
        null
    } else {
        val bytes = ByteArray(length.toInt())
        val complete = file.inputStream().use { input ->
            var offset = 0
            while (offset < bytes.size) {
                checkImageReadCancellation()
                val count = input.read(bytes, offset, minOf(DEFAULT_BUFFER_SIZE, bytes.size - offset))
                if (count < 0) return@use false
                offset += count
            }
            input.read() < 0
        }
        if (!complete) null else {
            val bounds = BitmapFactory.Options().apply { inJustDecodeBounds = true }
            BitmapFactory.decodeByteArray(bytes, 0, bytes.size, bounds)
            val mime = bounds.outMimeType?.takeIf { it.startsWith("image/") }
            if (bounds.outWidth <= 0 || bounds.outHeight <= 0 || mime == null) null else {
                val options = BitmapFactory.Options().apply {
                    inSampleSize = ImageArtifactProcessor.sampleSizeForBounds(
                        bounds.outWidth, bounds.outHeight, ImageArtifactProcessor.DEFAULT_LONG_EDGE,
                    )
                }
                val decoded = BitmapFactory.decodeByteArray(bytes, 0, bytes.size, options)
                if (decoded == null) null else {
                    decoded.recycle()
                    checkImageReadCancellation()
                    ImageSnapshot(bytes, mime)
                }
            }
        }
    }
} catch (error: CancellationException) {
    throw error
} catch (error: InterruptedIOException) {
    throw error
} catch (_: IOException) {
    null
} catch (_: IllegalArgumentException) {
    null
} catch (_: SecurityException) {
    null
}

private fun checkImageReadCancellation() {
    if (Thread.currentThread().isInterrupted) throw InterruptedIOException("Image read interrupted")
    if (HttpClient.isRequestCancelled()) throw CancellationException("Image read cancelled")
}

/**
 * The transport form of a prepared [StreamingJsonBody]: replaces every placeholder with the base64
 * of its file while OkHttp writes the request, so image bytes never become a String or a
 * whole-request byte array. The body reopens each immutable attachment on every write and is
 * therefore replayable for endpoint fallback and transport retries.
 */
internal fun StreamingJsonBody.toRequestBody(): RequestBody = ReplayableJsonRequestBody(this)

private class ReplayableJsonRequestBody(
    private val body: StreamingJsonBody,
) : RequestBody() {
    private val segments = body.segments
    private val parts = body.parts

    override fun contentType(): MediaType = JSON_MEDIA_TYPE

    override fun contentLength(): Long = body.contentLength

    override fun writeTo(sink: BufferedSink) {
        parts.forEachIndexed { index, part ->
            if (!part.metadataMatches()) {
                throw IOException("Image attachment changed before request upload")
            }
            sink.write(segments[index])
            val digest = MessageDigest.getInstance("SHA-256")
            val base64Output = Base64.getEncoder().wrap(
                NonClosingOutputStream(sink.outputStream()),
            )
            var completed = false
            try {
                part.file.inputStream().use { input ->
                    val buffer = ByteArray(DEFAULT_BUFFER_SIZE)
                    var remaining = part.byteCount
                    while (remaining > 0L) {
                        val read = input.read(
                            buffer,
                            0,
                            minOf(buffer.size.toLong(), remaining).toInt(),
                        )
                        if (read < 0) {
                            throw IOException("Image attachment changed during request upload")
                        }
                        digest.update(buffer, 0, read)
                        base64Output.write(buffer, 0, read)
                        remaining -= read
                    }
                    if (input.read() >= 0) {
                        throw IOException("Image attachment changed during request upload")
                    }
                }
                if (!part.metadataMatches() || !digest.digest().contentEquals(part.sha256)) {
                    throw IOException("Image attachment changed during request upload")
                }
                completed = true
            } finally {
                if (completed) base64Output.close()
            }
        }
        sink.write(segments.last())
    }
}

private class NonClosingOutputStream(
    private val delegate: OutputStream,
) : OutputStream() {
    override fun write(value: Int) = delegate.write(value)

    override fun write(buffer: ByteArray, offset: Int, length: Int) =
        delegate.write(buffer, offset, length)

    override fun flush() = delegate.flush()

    override fun close() = Unit
}
