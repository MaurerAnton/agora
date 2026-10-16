package com.newoether.agora.api.util

import okhttp3.MediaType
import okhttp3.MediaType.Companion.toMediaType
import okhttp3.RequestBody
import okio.BufferedSink
import java.io.IOException
import java.io.OutputStream
import java.security.MessageDigest
import java.util.Base64

private val JSON_MEDIA_TYPE: MediaType = "application/json; charset=utf-8".toMediaType()

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
