package com.newoether.agora.diagnostics

import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class DiagnosticRedactorTest {
    @Test
    fun `headers query parameters and user info are permanently redacted`() {
        val headers = DiagnosticRedactor.captureHeaders(
            mapOf(
                "Authorization" to "Bearer top-secret-token",
                "Cookie" to "session=cookie-secret",
                "X-Trace" to "api_key=another-secret",
            ),
        )
        val url = DiagnosticRedactor.captureUrl(
            "https://user:password@example.com/v1/chat" +
                "?X-Amz-Credential=signed-user&X-Amz-Signature=signed-secret&model=test",
        )

        assertEquals("[REDACTED_SECRET]", headers["Authorization"])
        assertEquals("[REDACTED_SECRET]", headers["Cookie"])
        assertFalse(headers.getValue("X-Trace").contains("another-secret"))
        assertFalse(url.value.contains("user"))
        assertFalse(url.value.contains("password"))
        assertFalse(url.value.contains("signed-user"))
        assertFalse(url.value.contains("signed-secret"))
        assertTrue(url.value.contains("model=test"))
    }

    @Test
    fun `raw capture keeps json content while removing credentials`() {
        val raw = """
            {
              "api_key": "nested-secret",
              "messages": [{"role": "user", "content": "private prompt"}],
              "max_tokens": 10,
              "nested": {"access_token": "token-secret", "text": "private text"}
            }
        """.trimIndent()

        val captured = DiagnosticRedactor.captureJson(raw)

        assertFalse(captured.value.contains("nested-secret"))
        assertFalse(captured.value.contains("token-secret"))
        assertTrue(captured.value.contains("private prompt"))
        assertTrue(captured.value.contains("private text"))
        assertTrue(captured.value.contains("[REDACTED_SECRET]"))
        assertTrue(captured.value.contains("max_tokens"))
        assertTrue(captured.value.contains("10"))
    }

    @Test
    fun `captured content keeps semantic text but removes inline credentials`() {
        val raw = """
            {
              "content": "keep this text but remove sk-abcdefghijklmnop",
              "authorization": "Bearer abcdefghijklmnop",
              "nested": {"password": "password-secret"}
            }
        """.trimIndent()

        val captured = DiagnosticRedactor.captureJson(raw)

        assertTrue(captured.value.contains("keep this text"))
        assertFalse(captured.value.contains("sk-abcdefghijklmnop"))
        assertFalse(captured.value.contains("abcdefghijklmnop"))
        assertFalse(captured.value.contains("password-secret"))
    }

    @Test
    fun `raw capture sanitizes ndjson wire lines without removing message content`() {
        val line = DiagnosticRedactor.captureWireLine(
            """{"message":{"content":"private local response"},"token":"secret-value"}""",
        )

        assertTrue(line.value.contains("private local response"))
        assertFalse(line.value.contains("secret-value"))
        assertTrue(line.value.contains("[REDACTED_SECRET]"))
    }

    @Test
    fun `malformed urls and short inline credentials fail closed`() {
        val url = DiagnosticRedactor.captureUrl(
            "not a valid url user:password",
        )
        val content = DiagnosticRedactor.captureContent(
            "token=x password=y Bearer z",
        )
        val privateKey = DiagnosticRedactor.captureContent(
            "visible\n-----BEGIN PRIVATE KEY-----\npartial-secret",
        )

        assertEquals("[UNAVAILABLE_INVALID_URL]", url.value)
        assertFalse(content.value.contains("token=x"))
        assertFalse(content.value.contains("password=y"))
        assertFalse(content.value.contains("Bearer z"))
        assertTrue(privateKey.value.contains("visible"))
        assertFalse(privateKey.value.contains("partial-secret"))
    }

    @Test
    fun `invalid json preserves noncredential text while removing labeled secrets`() {
        val body = DiagnosticRedactor.captureJson(
            "not-json token=private-secret visible text",
        )
        val line = DiagnosticRedactor.captureWireLine(
            "data: not-json token=private-secret visible text",
        )

        assertTrue(body.value.contains("visible text"))
        assertTrue(line.value.contains("visible text"))
        assertFalse(body.value.contains("private-secret"))
        assertFalse(line.value.contains("private-secret"))
    }

    @Test
    fun `redacted export projection removes semantic content but keeps structure`() {
        val captured = DiagnosticRedactor.captureJson(
            """{"messages":[{"role":"user","content":"private prompt"}],"arguments":"tool args","result":"tool result","max_tokens":10}""",
        )

        val redacted = DiagnosticRedactor.redactJsonContent(captured)

        assertFalse(redacted.value.contains("private prompt"))
        assertFalse(redacted.value.contains("tool args"))
        assertFalse(redacted.value.contains("tool result"))
        assertTrue(redacted.value.contains("[REDACTED_CONTENT]"))
        assertTrue(redacted.value.contains("max_tokens"))
        assertTrue(redacted.value.contains("10"))
    }

    @Test
    fun `large captured values use the two mebibyte utf8 limit`() {
        val privateContent = "x".repeat(DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES + 1)
        val captured = DiagnosticRedactor.captureContent(privateContent)

        assertTrue(captured.truncated)
        assertTrue(captured.originalLength > captured.value.length)
        assertEquals(DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES, captured.value.length)
    }
}
