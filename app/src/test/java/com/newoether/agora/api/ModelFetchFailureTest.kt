package com.newoether.agora.api

import java.io.IOException
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class ModelFetchFailureTest {
    @Test
    fun httpFailurePreservesStatusAndExtractsNestedProviderMessage() {
        val response = HttpClient.TextResponse(
            code = 401,
            body = """
                {
                  "error": {
                    "type": "authentication_error",
                    "message": "Invalid API key"
                  }
                }
            """.trimIndent(),
            isSuccessful = false,
        )

        val error = runCatching { response.requireModelFetchBody() }.exceptionOrNull()

        assertTrue(error is ModelFetchHttpException)
        error as ModelFetchHttpException
        assertEquals(401, error.statusCode)
        assertEquals("Invalid API key", error.detail)
        assertEquals("HTTP 401 — Invalid API key", error.message)
    }

    @Test
    fun rawProviderFailureIsCollapsedToOneBoundedLine() {
        val detail = extractModelFetchErrorDetail(
            "  first line\r\nsecond\tline " + "x".repeat(400),
        )

        assertTrue(detail?.startsWith("first line second line ") == true)
        assertTrue(detail?.contains('\n') == false)
        assertEquals(240, detail?.length)
        assertTrue(detail?.endsWith("…") == true)
    }

    @Test
    fun successfulEmptyBodyIsAnInvalidResponse() {
        val response = HttpClient.TextResponse(
            code = 200,
            body = "",
            isSuccessful = true,
        )

        val error = runCatching { response.requireModelFetchBody() }.exceptionOrNull()

        assertTrue(error is ModelFetchInvalidResponseException)
        assertEquals("Empty response body", error?.cause?.message)
    }

    @Test
    fun modelFetchFailuresNameTheEndpointThatFailed() {
        assertEquals(
            "HTTP 404 — Not found (https://api.example.com/v1/models)",
            ModelFetchHttpException(
                404,
                "Not found",
                "https://api.example.com/v1/models",
            ).message,
        )
        assertEquals(
            "HTTP 404 — Not found",
            ModelFetchHttpException(404, "Not found").message,
        )
    }

    @Test
    fun endpointAttachmentKeepsTheFirstEndpointAndOnlyTouchesModelFetchFailures() {
        val attached = ModelFetchHttpException(404, "Not found")
            .withModelFetchEndpoint("https://api.example.com/v1/models")

        assertEquals(
            "HTTP 404 — Not found (https://api.example.com/v1/models)",
            attached.message,
        )
        assertEquals(
            "HTTP 404 — Not found (https://api.example.com/v1/models)",
            attached.withModelFetchEndpoint("https://other.example.com/v1/models").message,
        )

        val invalid = ModelFetchInvalidResponseException(IllegalArgumentException("Expected JSON object"))
            .withModelFetchEndpoint("https://api.example.com/v1/models?key=SECRET")
        assertEquals(
            "Expected JSON object (https://api.example.com/v1/models)",
            invalid.message,
        )

        assertEquals(
            "boom",
            IOException("boom").withModelFetchEndpoint("https://api.example.com/v1/models").message,
        )
        assertEquals(
            "Request timed out",
            ModelFetchTimeoutException().withModelFetchEndpoint("https://api.example.com/v1/models").message,
        )
    }
}
