package com.newoether.agora.tool

import com.newoether.agora.util.Constants
import com.newoether.agora.util.ShellClient
import com.newoether.agora.util.ShellGrepResult
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertTrue
import org.junit.Test

class FileGrepJsonBoundTest {
    private fun match(i: Int) = ShellClient.GrepMatch(
        path = "F:\\Deploy\\strata\\serve\\server.py",
        line = i,
        content = "x".repeat(400),
        before = listOf("b".repeat(400), "b".repeat(400)),
        after = listOf("a".repeat(400), "a".repeat(400)),
    )

    private fun parse(json: String) = Json.parseToJsonElement(json) as JsonObject

    @Test
    fun smallResultIsUnchanged() {
        val grep = ShellGrepResult(matches = (1..3).map(::match), truncated = false)
        val obj = parse(fileGrepJson("p", grep, contextRequested = true))
        assertEquals(3, (obj["matches"] as JsonArray).size)
        assertEquals("false", (obj["truncated"] as JsonPrimitive).content)
    }

    // 500 matches with context exceed the stored-result cap; the JSON must stay parseable,
    // keep whole matches only and say it was truncated.
    @Test
    fun oversizedResultDropsWholeMatchesAndStaysValidJson() {
        val grep = ShellGrepResult(matches = (1..500).map(::match), truncated = false, skippedLargeFiles = 1)
        val json = fileGrepJson("Tokenizer|prompt_n", grep, contextRequested = true)
        assertTrue(json.length <= Constants.MAX_TOOL_RESULT_LENGTH)
        val obj = parse(json)
        val kept = obj["matches"] as JsonArray
        assertTrue(kept.size in 1 until 500)
        assertEquals("true", (obj["truncated"] as JsonPrimitive).content)
        assertEquals("1", (obj["skipped_large_files"] as JsonPrimitive).content)
        // Same clipping the generation pipeline applies must not change it.
        assertEquals(json, json.take(Constants.MAX_TOOL_RESULT_LENGTH))
    }

    @Test
    fun boundIsTight() {
        val grep = ShellGrepResult(matches = (1..50).map(::match), truncated = false)
        val limit = 5_000
        val json = fileGrepJson("p", grep, contextRequested = false, maxLength = limit)
        assertTrue(json.length <= limit)
        val kept = (parse(json)["matches"] as JsonArray).size
        val oneMore = fileGrepJson("p", grep.copy(matches = grep.matches.take(kept + 1)), false)
        // Adding the next whole match would have exceeded the limit.
        assertTrue(oneMore.length > limit)
    }
}
