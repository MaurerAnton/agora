package com.newoether.agora.remote

import com.sun.net.httpserver.HttpServer
import kotlinx.coroutines.runBlocking
import kotlinx.serialization.encodeToString
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import org.junit.Assert.*
import org.junit.Test
import java.net.InetSocketAddress

/** Application wire tests use production FiloClient validation, not permissive client mocks. */
class FiloControllerContractTest {
    private val token = "a".repeat(64)
    private val session = "00000000-0000-0000-0000-000000000001"
    private val input = "00000000-0000-0000-0000-000000000002"

    @Test fun catalogRetryThenCreationPreservesTheValidatedFirstTurnIdentity() = runBlocking {
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val paths = mutableListOf<String>()
        var catalogReads = 0
        server.createContext("/") { exchange ->
            paths += "${exchange.requestMethod} ${exchange.requestURI.path}"
            assertEquals("Bearer $token", exchange.requestHeaders.getFirst("Authorization"))
            val (status, body) = when (exchange.requestURI.path) {
                "/v1/models" -> if (++catalogReads == 1) 503 to "{}" else 200 to
                    """{"models":[{"id":"native-model","name":"Native","isDefault":true}]}"""
                else -> {
                    val request = Json.parseToJsonElement(exchange.requestBody.reader().readText()).jsonObject
                    assertEquals("native-model", request["settings"]!!.jsonObject["model"]!!.toString().trim('"'))
                    201 to """{"id":"$session","title":"New","cwd":"/native","updatedAt":1,"turnId":"first","clientId":"$input"}"""
                }
            }
            val bytes = body.toByteArray()
            exchange.sendResponseHeaders(status, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        try {
            val client = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
            try { client.models(); fail("The first catalog read must fail") } catch (_: FiloHttpException) { }
            try { client.create("hello", input); fail("Missing settings cannot create a native task") }
            catch (_: FiloInputException) { }
            assertEquals(listOf("GET /v1/models"), paths)
            val model = client.models().single()
            val created = client.create("hello", input, settings = RemoteSettings(model = model.id))
            assertEquals(session, created.id)
            assertEquals("first", created.acceptedTurnId)
            assertEquals(listOf("GET /v1/models", "GET /v1/models", "POST /v1/sessions"), paths)
        } finally { server.stop(0) }
    }

    @Test fun retiredNativeUnknownSettingsRemainUnknownUntilExplicitPatchAndReadBack() = runBlocking {
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val bodies = mutableListOf<String>()
        val model = RemoteModel("native-model", "Native", reasoningEfforts = listOf("none", "high"),
            serviceTiers = listOf(RemoteServiceTier("ultrafast", "Ultra Fast")))
        var runtime = RemoteRuntime("notLoaded", model = model.id)
        server.createContext("/") { exchange ->
            val body = if (exchange.requestMethod == "POST") {
                val patch = exchange.requestBody.reader().readText()
                bodies += patch
                assertEquals("{\"serviceTier\":null}", patch)
                runtime = runtime.copy(serviceTier = "default", serviceTierKnown = true)
                "{}"
            } else Json.encodeToString(RemoteConversationPage(emptyList(), null, emptyList(), runtime))
            val bytes = body.toByteArray()
            exchange.sendResponseHeaders(200, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        try {
            val client = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
            val before = RemoteState(deviceId = "device", session = RemoteSession(session, "Task", "/native", 1),
                models = listOf(model), runtime = client.conversation(session).runtime)
            assertTrue(before.canEditSettings)
            assertNull(before.selectedEffort)
            assertFalse(before.serviceTierKnown)
            client.updateSettings(session, RemoteSettings(updateServiceTier = true))
            val after = before.copy(runtime = client.conversation(session).runtime)
            assertTrue(after.serviceTierKnown)
            assertNull(after.selectedServiceTier)
            assertEquals(1, bodies.size)
        } finally { server.stop(0) }
    }

    @Test fun productionPageDecoderAcceptsOffPageProofAndRejectsMalformedOrUnboundedReceipts() = runBlocking {
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        var receipts = listOf(RemoteInputReceipt(input, "turn", "off-page-user"))
        server.createContext("/") { exchange ->
            val page = RemoteConversationPage(emptyList(), "older", emptyList(),
                RemoteRuntime("idle", completedTurnId = "turn"), inputReceipts = receipts)
            val bytes = Json.encodeToString(page).toByteArray()
            exchange.sendResponseHeaders(200, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        try {
            val client = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
            val page = client.conversation(session)
            assertTrue(page.nodes.isEmpty())
            assertEquals("off-page-user", page.inputReceipts.single().messageId)
            for (invalid in listOf(listOf(RemoteInputReceipt(input, "", "user")),
                listOf(receipts.single(), receipts.single()),
                List(129) { RemoteInputReceipt("input-$it", "turn", "user-$it") })) {
                receipts = invalid
                try { client.conversation(session); fail("Malformed native proof must fail") }
                catch (_: IllegalArgumentException) { }
            }
        } finally { server.stop(0) }
    }
}
