package com.newoether.agora.tool

import java.io.File
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Test

class ImageGenOpenRouterTest {
    @Test
    fun `openrouter base url is detected`() {
        assertTrue(isOpenRouterEndpoint("https://openrouter.ai/api/v1"))
        assertTrue(isOpenRouterEndpoint("https://openrouter.ai/api/v1/"))
        assertTrue(isOpenRouterEndpoint("HTTPS://OPENROUTER.AI/API/V1"))
        assertFalse(isOpenRouterEndpoint("https://api.openai.com/v1"))
        assertFalse(isOpenRouterEndpoint("http://localhost:11434/v1"))
        assertFalse(isOpenRouterEndpoint(""))
    }

    @Test
    fun `size maps to nearest supported aspect ratio`() {
        assertEquals("1:1", aspectRatioForSize("1024x1024"))
        assertEquals("3:2", aspectRatioForSize("1536x1024"))
        assertEquals("2:3", aspectRatioForSize("1024x1536"))
        assertEquals("16:9", aspectRatioForSize("1920x1080"))
        assertEquals("9:16", aspectRatioForSize("1080x1920"))
        assertEquals("4:3", aspectRatioForSize("1024x768"))
        assertEquals("3:4", aspectRatioForSize("768x1024"))
        assertEquals("auto", aspectRatioForSize("not-a-size"))
        assertEquals("auto", aspectRatioForSize(""))
        assertEquals("auto", aspectRatioForSize("0x0"))
    }

    @Test
    fun `provider branches endpoint body and headers for openrouter`() {
        val provider = source("tool/ImageGenToolProvider.kt")
        // OpenRouter Image API path, not OpenAI's /images/generations.
        assertTrue(provider.contains("\"\$baseUrl/images\""))
        assertTrue(provider.contains("isOpenRouterEndpoint(baseUrl)"))
        assertTrue(provider.contains("put(\"aspect_ratio\""))
        // OpenRouter requires attribution headers like the chat provider.
        assertTrue(provider.contains("HTTP-Referer"))
        assertTrue(provider.contains("X-Title"))
        // OpenAI path is preserved for non-OpenRouter bases.
        assertTrue(provider.contains("\"\$baseUrl/images/generations\""))
        assertTrue(provider.contains("put(\"size\", size)"))
    }

    private fun source(relativePath: String): String {
        var directory = File(requireNotNull(System.getProperty("user.dir"))).absoluteFile
        repeat(8) {
            val candidate = File(
                directory,
                "app/src/main/java/com/newoether/agora/$relativePath",
            )
            if (candidate.isFile) return candidate.readText().replace("\r\n", "\n")
            directory = directory.parentFile ?: return@repeat
        }
        error("Unable to locate Agora main sources")
    }
}
