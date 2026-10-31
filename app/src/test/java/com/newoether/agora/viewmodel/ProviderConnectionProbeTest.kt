package com.newoether.agora.viewmodel

import java.net.InetAddress
import java.net.ServerSocket
import java.util.Collections
import kotlin.concurrent.thread
import kotlinx.coroutines.runBlocking
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

class ProviderConnectionProbeTest {
    @Test
    fun `openai probe walks the version fallback and speaks bearer auth`() {
        val steps = connectionProbeSteps(
            wire = ProbeWire.OPENAI,
            baseUrl = "https://relay.example.com",
            apiKey = "key-1",
            chatModel = "relay:model-x",
        )

        assertEquals(2, steps.size)
        assertEquals(
            listOf(
                "https://relay.example.com/models",
                "https://relay.example.com/v1/models",
            ),
            steps[0].candidates.map(ProviderProbeRequest::url),
        )
        assertEquals(
            listOf(
                "https://relay.example.com/chat/completions",
                "https://relay.example.com/v1/chat/completions",
            ),
            steps[1].candidates.map(ProviderProbeRequest::url),
        )
        assertEquals("GET", steps[0].candidates.first().method)
        assertEquals("POST", steps[1].candidates.first().method)
        assertEquals("Bearer key-1", steps[0].candidates.first().headers["Authorization"])
        assertTrue(
            "the chat body carries the bare wire model id",
            steps[1].candidates.first().body.orEmpty()
                .contains("\"model\":\"model-x\""),
        )
    }

    @Test
    fun `a versioned anthropic base probes the versioned endpoint first and keeps the fallback`() {
        val steps = connectionProbeSteps(
            wire = ProbeWire.ANTHROPIC,
            baseUrl = "https://api.relay.example/anthropic/v1",
            apiKey = "key-2",
            chatModel = null,
        )

        assertEquals(1, steps.size)
        // Same candidates model sync walks: the configured versioned base first, then the
        // unversioned fallback it derives from it (customEndpointBaseUrlCandidates).
        assertEquals(
            listOf(
                "https://api.relay.example/anthropic/v1/models",
                "https://api.relay.example/anthropic/models",
            ),
            steps[0].candidates.map(ProviderProbeRequest::url),
        )
        val headers = steps[0].candidates.first().headers
        assertEquals("key-2", headers["x-api-key"])
        assertEquals("2023-06-01", headers["anthropic-version"])
        assertFalse(headers.containsKey("Authorization"))
    }

    @Test
    fun `google probe uses the gemini version path and never posts a chat turn`() {
        val unversioned = connectionProbeSteps(
            wire = ProbeWire.GOOGLE,
            baseUrl = "https://gateway.example.com",
            apiKey = "key-3",
            chatModel = "google:gemini-2.5-pro",
        )

        assertEquals(
            listOf("https://gateway.example.com/v1beta/models"),
            unversioned[0].candidates.map(ProviderProbeRequest::url),
        )
        assertEquals("key-3", unversioned[0].candidates.first().headers["x-goog-api-key"])
        assertEquals("only the model list is probed", 1, unversioned.size)

        val versioned = connectionProbeSteps(
            wire = ProbeWire.GOOGLE,
            baseUrl = "https://gateway.example.com/v1beta",
            apiKey = "key-3",
            chatModel = null,
        )
        assertEquals(
            listOf("https://gateway.example.com/v1beta/models"),
            versioned[0].candidates.map(ProviderProbeRequest::url),
        )
    }

    @Test
    fun `ollama probe speaks its own paths`() {
        val steps = connectionProbeSteps(
            wire = ProbeWire.OLLAMA,
            baseUrl = "http://localhost:11434",
            apiKey = "",
            chatModel = "llama3.2:3b",
        )

        assertEquals(
            listOf("http://localhost:11434/api/tags"),
            steps[0].candidates.map(ProviderProbeRequest::url),
        )
        assertEquals(
            listOf("http://localhost:11434/api/chat"),
            steps[1].candidates.map(ProviderProbeRequest::url),
        )
        assertTrue(steps[1].candidates.first().body.orEmpty().contains("\"stream\":false"))
    }

    @Test
    fun `stored model ids lose their provider namespace before they reach the wire`() {
        assertEquals("deepseek-v4.1-flash", probeModelId("relay:deepseek-v4.1-flash"))
        assertEquals("gemini-2.5-pro", probeModelId("google:models/gemini-2.5-pro"))
        assertEquals("claude-3-5-sonnet", probeModelId("claude-3-5-sonnet"))
        assertNull(probeModelId(null))
        assertNull(probeModelId("   "))
        assertNull(probeModelId(""))
    }

    @Test
    fun `an empty base url produces no requests`() {
        assertTrue(connectionProbeSteps(ProbeWire.OPENAI, "  ", "key", "model").isEmpty())
    }

    @Test
    fun `attempts render as copy-pasteable lines without credentials`() {
        val attempt = ProviderProbeAttempt(
            request = ProviderProbeRequest(
                method = "GET",
                url = "https://gateway.example.com/v1beta/models?key=SECRET",
            ),
            httpCode = 404,
            elapsedMillis = 35L,
            detail = "Not found",
        )

        assertEquals(
            "GET https://gateway.example.com/v1beta/models → HTTP 404 · 35 ms · Not found",
            attempt.render(),
        )
        assertFalse(attempt.successful)

        val unreachable = ProviderProbeAttempt(
            request = ProviderProbeRequest(method = "POST", url = "https://relay.example.com/v1/chat/completions"),
            httpCode = 0,
            elapsedMillis = 12L,
            detail = "SocketTimeoutException: timeout",
        )
        assertEquals(
            "POST https://relay.example.com/v1/chat/completions → no response · 12 ms · " +
                "SocketTimeoutException: timeout",
            unreachable.render(),
        )
        assertTrue(
            ProviderProbeReport(listOf(attempt.copy(httpCode = 200))).successful,
        )
        assertFalse(ProviderProbeReport(listOf(attempt)).successful)
    }

    @Test
    fun `a 404 moves to the versioned candidate while any other answer stops the step`() = runBlocking {
        val server = ServerSocket(0, 2, InetAddress.getByName("127.0.0.1")).apply {
            soTimeout = 5_000
        }
        val seen = Collections.synchronizedList(mutableListOf<String>())
        val worker = serve(server, seen, listOf(404 to "Not found", 200 to "{}"))
        try {
            val base = "http://127.0.0.1:${server.localPort}"
            val report = runConnectionProbe(
                connectionProbeSteps(ProbeWire.OPENAI, base, "key", chatModel = null),
            )

            assertEquals(listOf(404, 200), report.attempts.map(ProviderProbeAttempt::httpCode))
            assertTrue(report.successful)
            assertEquals(2, seen.size)
            assertTrue(seen[0].startsWith("GET /models HTTP/1.1"))
            assertTrue(seen[1].startsWith("GET /v1/models HTTP/1.1"))
            assertEquals("Not found", report.attempts.first().detail)
        } finally {
            server.close()
            worker.join(2_000)
        }
    }

    @Test
    fun `an authenticated answer stops the step instead of probing a second path`() = runBlocking {
        val server = ServerSocket(0, 2, InetAddress.getByName("127.0.0.1")).apply {
            soTimeout = 5_000
        }
        val seen = Collections.synchronizedList(mutableListOf<String>())
        val worker = serve(server, seen, listOf(401 to """{"error":"Authentication required"}"""))
        try {
            val base = "http://127.0.0.1:${server.localPort}"
            val report = runConnectionProbe(
                connectionProbeSteps(ProbeWire.OPENAI, base, "key", chatModel = null),
            )

            assertEquals(listOf(401), report.attempts.map(ProviderProbeAttempt::httpCode))
            assertEquals(1, seen.size)
            assertFalse(report.successful)
            assertEquals(
                "Authentication required",
                report.attempts.single().detail,
            )
        } finally {
            server.close()
            worker.join(2_000)
        }
    }

    /** Serves [responses] in order and records every received request line. */
    private fun serve(
        server: ServerSocket,
        seen: MutableList<String>,
        responses: List<Pair<Int, String>>,
    ) = thread(name = "probe-test-server", isDaemon = true) {
        runCatching {
            responses.forEach { (code, body) ->
                server.accept().use { socket ->
                    val reader = socket.getInputStream().bufferedReader()
                    seen += reader.readLine().orEmpty()
                    while (true) {
                        val line = reader.readLine() ?: break
                        if (line.isEmpty()) break
                    }
                    val reason = when (code) {
                        200 -> "OK"
                        404 -> "Not Found"
                        401 -> "Unauthorized"
                        else -> "Error"
                    }
                    val payload = body.toByteArray()
                    val response = buildString {
                        append("HTTP/1.1 $code $reason\r\n")
                        append("Content-Type: application/json\r\n")
                        append("Content-Length: ${payload.size}\r\n")
                        append("Connection: close\r\n\r\n")
                    }.toByteArray() + payload
                    socket.getOutputStream().apply {
                        write(response)
                        flush()
                    }
                }
            }
        }
    }
}
