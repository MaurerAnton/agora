package com.newoether.agora.api.util

import com.newoether.agora.model.AttachmentItem
import com.newoether.agora.model.AttachmentMeta
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.ImageArtifact
import com.newoether.agora.model.Participant
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import okio.Buffer
import okio.ForwardingSink
import okio.buffer
import org.junit.Assert.assertArrayEquals
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertThrows
import org.junit.Assert.assertTrue
import org.junit.Test
import java.io.File
import java.io.IOException
import java.nio.file.Files
import java.util.Base64
import java.util.concurrent.Callable
import java.util.concurrent.Executors
import java.util.concurrent.TimeUnit

class StreamingJsonRequestBodyTest {
    @Test
    fun manyFilesStreamWithExactLengthAndReplayIdentically() {
        val directory = Files.createTempDirectory("agora-streaming-json-").toFile()
        try {
            val registry = Base64FileRegistry()
            val payloads = listOf(ByteArray(0)) + (0 until 64).map { index ->
                ByteArray(4_097 + index) { offset -> ((index * 31 + offset) and 0xff).toByte() }
            }
            val placeholders = payloads.mapIndexed { index, payload ->
                val file = File(directory, "frame-$index.jpg").apply { writeBytes(payload) }
                checkNotNull(registry.register(file.absolutePath))
            }
            val template = placeholders.joinToString(
                prefix = "{\"note\":\"图像🙂\",\"images\":[\"",
                separator = "\",\"",
                postfix = "\"]}",
            )
            val body = checkNotNull(registry.prepare(template))
            val request = body.toRequestBody()

            val first = Buffer().also(request::writeTo).readByteArray()
            val second = Buffer().also(request::writeTo).readByteArray()

            assertArrayEquals(first, second)
            assertEquals(first.size.toLong(), body.contentLength)
            val images = Json.parseToJsonElement(first.decodeToString())
                .jsonObject.getValue("images").jsonArray
            assertEquals(65, images.size)
            payloads.forEachIndexed { index, payload ->
                assertEquals(
                    Base64.getEncoder().encodeToString(payload),
                    images[index].jsonPrimitive.content,
                )
            }
            assertTrue(body.diagnosticJson.contains("[STREAMED_BASE64:"))
            assertTrue(body.diagnosticJson.contains("_BYTES]"))
            assertFalse(body.diagnosticJson.contains("__AGORA_BASE64_"))
            assertFalse(body.diagnosticJson.contains(images[1].jsonPrimitive.content))
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test
    fun sameLengthReplacementIsRejectedEvenWhenTimestampIsRestored() {
        val file = File.createTempFile("agora-stream-replay-", ".jpg")
        try {
            val original = ByteArray(8_193) { index -> (index and 0xff).toByte() }
            file.writeBytes(original)
            val originalTimestamp = file.lastModified()
            val registry = Base64FileRegistry()
            val placeholder = checkNotNull(registry.register(file.absolutePath))
            val request = checkNotNull(
                registry.prepare("{\"image\":\"$placeholder\"}"),
            ).toRequestBody()
            request.writeTo(Buffer())

            file.writeBytes(original.map { byte -> (byte.toInt() xor 0x5a).toByte() }.toByteArray())
            assertTrue(file.setLastModified(originalTimestamp))
            assertEquals(original.size.toLong(), file.length())
            assertEquals(originalTimestamp, file.lastModified())

            assertThrows(IOException::class.java) {
                request.writeTo(Buffer())
            }
        } finally {
            file.delete()
        }
    }

    @Test
    fun missingFilesAreNotRegistered() {
        val registry = Base64FileRegistry()
        assertEquals(null, registry.register("Z:/agora/missing/image.png"))
        assertEquals(null, registry.prepare("{\"images\":[]}"))
    }

    @Test
    fun concurrentReplayKeepsEveryOccurrence() {
        val directory = Files.createTempDirectory("agora-streaming-json-").toFile()
        try {
            val registry = Base64FileRegistry()
            val payloads = (0 until 24).map { index ->
                ByteArray(97 + index) { offset -> ((index + offset) and 0xff).toByte() }
            }
            val placeholders = payloads.mapIndexed { index, payload ->
                val file = File(directory, "frame-$index.jpg").apply { writeBytes(payload) }
                checkNotNull(registry.register(file.absolutePath))
            }
            val template = placeholders.joinToString(
                prefix = "{\"content\":[\"",
                separator = "\",\"",
                postfix = "\"]}",
            )
            val request = envelope(checkNotNull(registry.prepare(template)))
            val executor = Executors.newFixedThreadPool(4)
            try {
                val results = executor.invokeAll((0 until 4).map { Callable { write(request).toString() } })
                val expected = results.first().get(10, TimeUnit.SECONDS)
                results.forEach { assertEquals(expected, it.get(10, TimeUnit.SECONDS)) }
                val parts = Json.parseToJsonElement(expected).jsonObject.getValue("content").jsonArray
                assertEquals(24, parts.size)
                payloads.forEachIndexed { index, payload ->
                    assertEquals(
                        Base64.getEncoder().encodeToString(payload),
                        parts[index].jsonPrimitive.content,
                    )
                }
            } finally {
                executor.shutdownNow()
            }
        } finally {
            directory.deleteRecursively()
        }
    }

    @Test
    fun writeFailuresAreNeverSwallowed() {
        val file = File.createTempFile("agora-stream-write-failure-", ".jpg")
        try {
            file.writeBytes(ByteArray(64) { 7 })
            val registry = Base64FileRegistry()
            val placeholder = checkNotNull(registry.register(file.absolutePath))
            val request = envelope(checkNotNull(registry.prepare("{\"image\":\"$placeholder\"}")))
            val sink = object : ForwardingSink(Buffer()) {
                override fun write(source: Buffer, byteCount: Long) {
                    throw IOException("network failed")
                }
            }.buffer()
            assertThrows(IOException::class.java) {
                request.body.writeTo(sink)
                sink.flush()
            }
        } finally {
            file.delete()
        }
    }

    @Test
    fun duplicatePathsKeepOccurrenceAvailabilityIndependent() {
        val artifact = ImageArtifact(
            path = "/tmp/agora-image.png", mimeType = "image/png", sizeBytes = 1L,
            width = 17, height = 11, policyVersion = 1,
        )
        val message = ChatMessage(
            text = "", participant = Participant.USER, images = listOf(artifact.path, artifact.path),
            attachmentMeta = AttachmentMeta(listOf(
                AttachmentItem(type = "image", imageIndex = 0, imageArtifacts = listOf(artifact)),
                AttachmentItem(
                    type = "image", imageIndex = 1,
                    imageArtifacts = listOf(artifact.copy(unavailable = true)),
                ),
            )),
        )
        assertEquals(listOf(artifact), message.imageArtifactsForSend())
    }

    private fun envelope(body: StreamingJsonBody): StreamingJsonRequest =
        StreamingJsonRequest(body = body.toRequestBody(), diagnosticJson = body.diagnosticJson)

    private fun write(request: StreamingJsonRequest): JsonObject =
        Json.parseToJsonElement(Buffer().also(request.body::writeTo).readUtf8()).jsonObject
}
