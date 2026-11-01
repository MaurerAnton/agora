package com.newoether.agora.remote

import com.sun.net.httpserver.HttpServer
import java.io.File
import java.net.InetSocketAddress
import kotlinx.coroutines.runBlocking
import org.junit.Assert.*
import org.junit.Assume.assumeTrue
import org.junit.Test
import org.junit.runner.RunWith
import org.robolectric.RobolectricTestRunner
import org.robolectric.annotation.Config

@RunWith(RobolectricTestRunner::class)
@Config(sdk = [35], application = android.app.Application::class)
class FiloUsageTest {
    @Test fun nativeUsageKeepsZeroAndMissingWindowsOnTheSelectedRoute() = runBlocking {
        val requests = mutableListOf<String>()
        val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val token = "a".repeat(64)
        server.createContext("/") { exchange ->
            requests += "${exchange.requestMethod} ${exchange.requestURI.path}"
            assertEquals("Bearer $token", exchange.requestHeaders.getFirst("Authorization"))
            val bytes = """{"limits":[{"id":"codex","name":"Codex","primary":{"usedPercent":0,"windowDurationMins":10080,"resetsAt":1793082762},"secondary":null}]}""".toByteArray()
            exchange.sendResponseHeaders(200, bytes.size.toLong())
            exchange.responseBody.use { it.write(bytes) }
        }
        server.start()
        try {
            val client = applicationFixtureClient("http://127.0.0.1:${server.address.port}/", token)
            for (selected in listOf(client, client.scoped(RemoteAccessPath("codex-desktop", nativeScopeId = "account-a")))) {
                val limit = selected.usage().limits.single()
                assertEquals(100, limit.primary!!.remainingPercent)
                assertEquals(10080L, limit.primary.windowDurationMins)
                assertNull(limit.secondary)
            }
            assertEquals(listOf("GET /v1/usage", "GET /v1/adapters/codex-desktop/usage"), requests)
        } finally { server.stop(0) }
    }

    /** Explicit opt-in, account reads only; no session or native execution requests. */
    @Test fun existingGatewayReturnsUsageThroughTheProductionEncryptedClient() = runBlocking {
        val address = System.getenv("FILO_USAGE_TEST_URL")
        val tokenFile = System.getenv("FILO_USAGE_TEST_TOKEN_FILE")
        assumeTrue("Opt-in read-only account probe", !address.isNullOrBlank() && !tokenFile.isNullOrBlank())
        val client = FiloClient(address!!, File(tokenFile!!).readText().trim())
        client.connect()
        val source = client.adapters.orEmpty().firstOrNull { it.family == "codex" && it.origin == "desktop" }
        assertNotNull("Desktop account source is missing", source)
        assertTrue("Desktop account usage is not supported", source!!.capabilities.usage)
        val selected = if (source.legacy) client else client.scoped(RemoteAccessPath(source.id, nativeScopeId = source.nativeScopeId))
        val usage = selected.usage()
        assertTrue("Native account returned no usage buckets", usage.limits.isNotEmpty())
        assertTrue("Native account returned no usage windows", usage.limits.any { it.primary != null || it.secondary != null })
    }
}
