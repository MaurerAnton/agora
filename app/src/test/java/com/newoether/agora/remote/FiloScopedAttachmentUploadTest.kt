package com.newoether.agora.remote

import com.newoether.agora.model.SelectedAttachment
import com.sun.net.httpserver.HttpServer
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.*
import org.junit.Rule
import org.junit.Test
import org.junit.rules.TemporaryFolder
import java.io.ByteArrayOutputStream
import java.net.InetSocketAddress
import java.util.Base64
import java.util.concurrent.ConcurrentLinkedQueue
import javax.crypto.Mac
import javax.crypto.spec.SecretKeySpec

class FiloScopedAttachmentUploadTest {
    @get:Rule val temporary = TemporaryFolder()
    private val json = Json { ignoreUnknownKeys = true }
    private val source = "codex-desktop"
    private val scope = "verified-account-scope"
    private val binding = "captured-session-binding"
    private val token = "a".repeat(64)
    private val session = "11111111-1111-4111-8111-111111111111"
    private val clientId = "22222222-2222-4222-8222-222222222222"

    private fun sealedUpload(): String {
        val data = Json.encodeToString(listOf("upload", source, scope, "", "b".repeat(32))).toByteArray()
        val encoder = Base64.getUrlEncoder().withoutPadding()
        val mac = Mac.getInstance("HmacSHA256")
        mac.init(SecretKeySpec(token.toByteArray(), "HmacSHA256"))
        return "filo.scope.${encoder.encodeToString(data)}.${encoder.encodeToString(mac.doFinal(data))}"
    }

    @Test fun scopedUploadCompletesWithoutNativePathThenSendAndCreateKeepTheirAuthority() = runBlocking {
        val bytes = ByteArray(600_003) { it.toByte() }
        val file = temporary.newFile("original.bin").apply { writeBytes(bytes) }
        val item = SelectedAttachment(uri = file.toURI().toString(), type = "file", fileName = file.name,
            mimeType = "application/octet-stream", fileSize = file.length(), localPath = file.path)
        val uploaded = ByteArrayOutputStream()
        val failures = ConcurrentLinkedQueue<Throwable>()
        val mutations = ConcurrentLinkedQueue<Pair<String, JsonObject>>()
        val uploadId = sealedUpload()
        var metadata: RemoteUpload? = null
        var chunkCount = 0
        val prefix = "/v1/adapters/$source"
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/") { exchange ->
            try {
                assertEquals("Bearer $token", exchange.requestHeaders.getFirst("Authorization"))
                val path = exchange.requestURI.path
                val response = when {
                    path == "$prefix/uploads" -> {
                        assertEquals("POST", exchange.requestMethod)
                        val input = json.parseToJsonElement(exchange.requestBody.bufferedReader().readText()) as JsonObject
                        assertEquals(scope, input["nativeScopeId"]!!.jsonPrimitive.content)
                        assertNull(input["accessBinding"])
                        metadata = json.decodeFromString<RemoteUpload>(input.toString()).copy(id = uploadId)
                        json.encodeToString(metadata!!)
                    }
                    path == "$prefix/uploads/$uploadId" -> {
                        assertEquals("PUT", exchange.requestMethod)
                        assertEquals("offset=${uploaded.size()}", exchange.requestURI.rawQuery)
                        val chunk = exchange.requestBody.readBytes()
                        assertTrue(chunk.size <= 256 * 1024)
                        uploaded.write(chunk)
                        chunkCount++
                        "{\"offset\":${uploaded.size()}}"
                    }
                    path == "$prefix/uploads/$uploadId/complete" -> {
                        assertEquals("POST", exchange.requestMethod)
                        assertArrayEquals(bytes, uploaded.toByteArray())
                        json.encodeToString(metadata!!)
                    }
                    path == "$prefix/sessions/$session/messages" || path == "$prefix/sessions" -> {
                        assertEquals("POST", exchange.requestMethod)
                        val input = json.parseToJsonElement(exchange.requestBody.bufferedReader().readText()) as JsonObject
                        assertEquals(uploadId, input["attachments"]!!.jsonArray.single().jsonPrimitive.content)
                        assertEquals(clientId, input["clientId"]!!.jsonPrimitive.content)
                        mutations.add(path to input)
                        if (path.endsWith("/messages")) {
                            assertEquals(binding, input["accessBinding"]!!.jsonPrimitive.content)
                            assertNull(input["nativeScopeId"])
                            """{"turnId":"native-turn","clientId":"$clientId"}"""
                        } else {
                            assertEquals(scope, input["nativeScopeId"]!!.jsonPrimitive.content)
                            assertNull(input["accessBinding"])
                            val access = RemoteSessionAccess(scope, "canonical", binding,
                                RemoteCapabilities(create = true, idleSend = true, uploads = true))
                            """{"id":"$session","title":"Task","cwd":"","updatedAt":1,"turnId":"native-turn","clientId":"$clientId","access":${Json.encodeToString(access)}}"""
                        }
                    }
                    else -> error("Unexpected request: $path")
                }.toByteArray()
                exchange.sendResponseHeaders(200, response.size.toLong())
                exchange.responseBody.use { it.write(response) }
            } catch (error: Throwable) {
                failures.add(error)
                runCatching { exchange.sendResponseHeaders(500, -1) }
            } finally { exchange.close() }
        }
        server.start()
        try {
            val client = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
                .scoped(RemoteAccessPath(source, binding, scope))
            val receipt = client.upload(item)
            assertEquals(uploadId, receipt.id)
            assertNull(receipt.path)
            assertEquals(3, chunkCount)
            assertEquals("native-turn", client.send(session, "Read", clientId, listOf(receipt.id)))
            assertEquals(session, client.create("Read", clientId, listOf(receipt.id)).id)
            assertEquals(2, mutations.size)
            assertTrue(failures.toString(), failures.isEmpty())
        } finally { server.stop(0) }
    }

    @Test fun malformedAndCrossProtocolUploadIdsCannotStartSendOrCreate() = runBlocking {
        var requests = 0
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        server.createContext("/") { exchange -> requests++; exchange.sendResponseHeaders(500, -1); exchange.close() }
        server.start()
        try {
            val legacy = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
            val scoped = legacy.scoped(RemoteAccessPath(source, binding, scope))
            val invalid = listOf("../elsewhere", "filo.scope.a.${"b".repeat(42)}",
                "filo.scope.a/b.${"b".repeat(43)}", "filo.scope.a.${"b".repeat(43)}?offset=0",
                "filo.scope.${"x".repeat(24576)}.${"b".repeat(43)}", "a".repeat(32))
            for (id in invalid) {
                assertTrue(runCatching { scoped.send(session, "Read", clientId, listOf(id)) }.exceptionOrNull() is FiloInputException)
                assertTrue(runCatching { scoped.create("Read", clientId, listOf(id)) }.exceptionOrNull() is FiloInputException)
            }
            assertTrue(runCatching { legacy.send(session, "Read", clientId, listOf(sealedUpload())) }
                .exceptionOrNull() is FiloInputException)
            assertTrue(runCatching { legacy.create("Read", clientId, listOf(sealedUpload())) }
                .exceptionOrNull() is FiloInputException)
            assertEquals(0, requests)
        } finally { server.stop(0) }
    }
}
