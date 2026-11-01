package com.newoether.agora.webui

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import org.junit.Assert.assertEquals
import org.junit.Assert.assertNull
import org.junit.Test

class WebJsonPatchTest {
    private fun parse(text: String): JsonElement = Json.parseToJsonElement(text)

    @Test
    fun unchangedFrameHasNoPatch() {
        val frame = parse("""{"id":"m","text":{"markdown":"abc","math":[]}}""")
        assertNull(webJsonPatch(frame, frame))
    }

    @Test
    fun grownTextTravelsAsItsSuffixOnly() {
        val patch = webJsonPatch(
            parse("""{"id":"m","status":"SENDING","text":{"markdown":"Hello","math":[]}}"""),
            parse("""{"id":"m","status":"SENDING","text":{"markdown":"Hello world","math":[]}}"""),
        )
        assertEquals(parse("""{"o":{"text":{"o":{"markdown":{"a":" world"}}}}}"""), patch)
    }

    @Test
    fun listsGrowAtTheTailAndPatchChangedEntriesInPlace() {
        val patch = webJsonPatch(
            parse("""{"blocks":[{"k":"a","t":"x"},{"k":"b","t":"y"}]}"""),
            parse("""{"blocks":[{"k":"a","t":"x"},{"k":"b","t":"yz"},{"k":"c","t":"w"}]}"""),
        )
        assertEquals(
            parse("""{"o":{"blocks":{"l":3,"i":{"1":{"o":{"t":{"a":"z"}}},"2":{"v":{"k":"c","t":"w"}}}}}}"""),
            patch,
        )
    }

    @Test
    fun shrunkListsRewrittenStringsAndRemovedMembersAreExplicit() {
        val patch = webJsonPatch(
            parse("""{"items":[1,2,3],"label":"abc","usage":{"t":1}}"""),
            parse("""{"items":[1,2],"label":"abd"}"""),
        )
        assertEquals(
            parse("""{"o":{"items":{"l":2},"label":{"v":"abd"}},"d":["usage"]}"""),
            patch,
        )
    }

    @Test
    fun typeChangesAndNewMembersAreReplaced() {
        val patch = webJsonPatch(
            parse("""{"presentation":null,"n":1}"""),
            parse("""{"presentation":{"blocks":[]},"n":2,"compact":true}"""),
        )
        assertEquals(
            parse("""{"o":{"presentation":{"v":{"blocks":[]}},"n":{"v":2},"compact":{"v":true}}}"""),
            patch,
        )
    }
}
