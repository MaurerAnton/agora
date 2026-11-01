package com.newoether.agora.tool

import com.newoether.agora.util.ShellFileReadResult
import io.mockk.coEvery
import io.mockk.coVerify
import io.mockk.every
import io.mockk.mockk
import io.mockk.verify
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.*
import org.junit.Test

class ShellFileReadTest {
    @Test
    fun relativeMarkdownStillReadsDiskAndAddsMemoryHintInBothModes() = runTest {
        for (mode in listOf("", ",\"start_line\":1,\"max_lines\":1")) {
            val backend = mockk<Backend>(relaxed = true)
            every { backend.device } returns null
            coEvery { backend.fileRead("notes.md", any(), any()) } returns ShellFileReadResult(
                content = "disk text\n", lines = 1, totalLines = 1, totalBytes = 10,
                returnedBytes = 10, offset = 0, limit = 1_048_576, truncated = false,
            )
            val output = executeDiskFileRead(
                """{"server":"device","path":"notes.md"$mode}""",
                openBackend = { name -> assertEquals("device", name); backend },
                missingServer = { "missing $it" },
            )
            val root = Json.parseToJsonElement(output).jsonObject
            assertTrue(output.contains("disk text"))
            assertTrue(root["hint"]!!.jsonPrimitive.content.contains("memory_read"))
            coVerify(atLeast = 1) { backend.fileRead("notes.md", any(), any()) }
            verify(exactly = 1) { backend.close() }
        }
    }

    @Test
    fun missingDeviceHasHintButExplicitPathsAndOtherExtensionsDoNot() = runTest {
        val output = executeDiskFileRead("""{"path":"missing.md"}""", { null }, { "missing" })
        assertTrue(output.contains("memory_read"))
        for (path in listOf("/tmp/notes.md", "C:\\notes.md", "notes.txt")) {
            assertEquals("{}", withMemoryEntryHint(path, "{}"))
        }
    }

    @Test
    fun cancellationClosesBackendAndPropagatesInBothModes() = runTest {
        for (mode in listOf("", ",\"start_line\":1")) {
            val backend = mockk<Backend>(relaxed = true)
            every { backend.device } returns null
            coEvery { backend.fileRead(any(), any(), any()) } throws CancellationException("stop")
            try {
                executeDiskFileRead("""{"path":"notes.md"$mode}""", { backend }, { "missing" })
                fail("Cancellation must propagate")
            } catch (_: CancellationException) {
                verify(exactly = 1) { backend.close() }
            }
        }
    }
}
