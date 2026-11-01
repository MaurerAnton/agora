package com.newoether.agora.tool

import com.newoether.agora.viewmodel.AskUserController
import com.newoether.agora.viewmodel.GenerationContext
import kotlinx.coroutines.ExperimentalCoroutinesApi
import kotlinx.coroutines.async
import kotlinx.coroutines.cancelAndJoin
import kotlinx.coroutines.launch
import kotlinx.coroutines.test.advanceTimeBy
import kotlinx.coroutines.test.runCurrent
import kotlinx.coroutines.test.runTest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertNull
import org.junit.Assert.assertTrue
import org.junit.Test

/**
 * What the model gets back from `ask_user`, and what it leaves on screen.
 *
 * A single question and a set of questions are the same tool call with different arguments, so both
 * shapes are pinned here, along with configured expiry, invalid arguments and generation stop.
 */
@OptIn(ExperimentalCoroutinesApi::class)
class AskUserToolProviderTest {

    private val ctx = GenerationContext(askUserEnabled = true, conversationId = "c1")

    private fun body(text: String): JsonObject = Json.parseToJsonElement(text).jsonObject

    private fun choicesOf(entry: JsonObject): List<String> =
        entry["choices"]?.jsonArray?.map { it.jsonPrimitive.content }.orEmpty()

    @Test
    fun `a single question answers with the flat shape`() = runTest {
        val controller = AskUserController()
        val provider = AskUserToolProvider(controller)
        val result = async {
            provider.execute("ask_user", """{"question":"Which one?","options":["A","B"]}""", ctx)
        }
        runCurrent()

        val request = controller.requests.value.single()
        assertEquals("Which one?", request.question)
        controller.submit(request.id, listOf("B"))

        val answer = body(result.await())
        assertTrue(answer["answered"]!!.jsonPrimitive.boolean)
        assertEquals(listOf("B"), choicesOf(answer))
        assertNull(answer["answers"])
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `blocking calls expire at the default five minutes and configured one or sixty minutes`() = runTest {
        val cases = listOf(
            ctx to 300_000L,
            ctx.copy(askUserTimeoutMinutes = 1) to 60_000L,
            ctx.copy(askUserTimeoutMinutes = 60) to 3_600_000L,
        )
        for ((context, timeoutMs) in cases) {
            val controller = AskUserController()
            val startedAt = testScheduler.currentTime
            val call = async {
                AskUserToolProvider(controller).execute("ask_user", """{"question":"Choose?","options":["A"]}""", context)
            }
            runCurrent()
            val request = controller.requests.value.single()

            advanceTimeBy(timeoutMs - 1L)
            runCurrent()
            assertFalse("Expired before ${context.askUserTimeoutMinutes} minutes", call.isCompleted)
            assertEquals(listOf(request), controller.requests.value)
            advanceTimeBy(1L)
            runCurrent()

            assertTrue("Still waiting at ${context.askUserTimeoutMinutes} minutes", call.isCompleted)
            val answer = body(call.await())
            assertFalse(answer["answered"]!!.jsonPrimitive.boolean)
            assertNull(answer["choices"])
            assertNull(answer["text"])
            assertNull(answer["answers"])
            assertEquals(timeoutMs, testScheduler.currentTime - startedAt)
            assertTrue(controller.requests.value.isEmpty())
        }
    }

    @Test
    fun `group expiry preserves later submitted answers and leaves the first unanswered`() = runTest {
        val controller = AskUserController()
        val result = async {
            AskUserToolProvider(controller).execute(
                "ask_user",
                """{"questions":[{"question":"First?","options":["A"]},
                    {"question":"Second?"},{"question":"Third?","options":["C"]}]}""",
                ctx.copy(askUserTimeoutMinutes = 1),
            )
        }
        runCurrent()
        val waiting = controller.requests.value
        advanceTimeBy(30_000L)
        controller.submit(waiting[1].id, emptyList(), "typed answer")
        advanceTimeBy(29_999L)
        controller.submit(waiting[2].id, listOf("C"))
        runCurrent()
        assertFalse(result.isCompleted)
        advanceTimeBy(1L)
        runCurrent()

        assertTrue(result.isCompleted)
        val answers = body(result.await())["answers"]!!.jsonArray.map { it.jsonObject }
        assertEquals(listOf("First?", "Second?", "Third?"), answers.map { it["question"]!!.jsonPrimitive.content })
        assertEquals(listOf(false, true, true), answers.map { it["answered"]!!.jsonPrimitive.boolean })
        assertTrue(choicesOf(answers[0]).isEmpty())
        assertNull(answers[0]["text"])
        assertEquals("typed answer", answers[1]["text"]!!.jsonPrimitive.content)
        assertEquals(listOf("C"), choicesOf(answers[2]))
        assertEquals(60_000L, testScheduler.currentTime)
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `a set of questions reaches the bar together with its own options`() = runTest {
        val controller = AskUserController()
        val provider = AskUserToolProvider(controller)
        val result = async {
            provider.execute(
                "ask_user",
                """
                {"questions":[
                  {"question":"First?","options":["A"]},
                  {"question":"Second?","options":["B","C"],"allow_multiple":true}
                ]}
                """.trimIndent(),
                ctx,
            )
        }
        runCurrent()

        val waiting = controller.requests.value
        assertEquals(listOf("First?", "Second?"), waiting.map { it.question })
        assertFalse(waiting[0].allowMultiple)
        assertTrue(waiting[1].allowMultiple)
        // One call, one card: both questions carry the same set id.
        assertTrue(waiting[0].setId != null)
        assertEquals(waiting[0].setId, waiting[1].setId)

        // Answered out of order, because both cards are on screen at the same time.
        controller.submit(waiting[1].id, listOf("B", "C"))
        controller.submit(waiting[0].id, emptyList(), "my own words")

        val answers = body(result.await())["answers"]!!.jsonArray.map { it.jsonObject }
        assertEquals(listOf("First?", "Second?"), answers.map { it["question"]!!.jsonPrimitive.content })
        assertEquals("my own words", answers[0]["text"]?.jsonPrimitive?.content)
        assertEquals(listOf("B", "C"), choicesOf(answers[1]))
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `skipping one question of a set does not hide the others`() = runTest {
        val controller = AskUserController()
        val provider = AskUserToolProvider(controller)
        val result = async {
            provider.execute(
                "ask_user",
                """{"questions":[{"question":"First?"},{"question":"Second?","options":["B"]}]}""",
                ctx,
            )
        }
        runCurrent()

        val waiting = controller.requests.value
        controller.dismiss(waiting[0].id)
        controller.submit(waiting[1].id, listOf("B"))

        val answers = body(result.await())["answers"]!!.jsonArray.map { it.jsonObject }
        assertFalse(answers[0]["answered"]!!.jsonPrimitive.boolean)
        assertTrue(answers[1]["answered"]!!.jsonPrimitive.boolean)
        assertEquals(listOf("B"), choicesOf(answers[1]))
    }

    @Test
    fun `asking one question and a set in the same call is refused`() = runTest {
        val controller = AskUserController()
        val result = AskUserToolProvider(controller).execute(
            "ask_user",
            """{"question":"Which one?","questions":[{"question":"Or this one?"}]}""",
            ctx,
        )
        assertEquals("ambiguous_questions", body(result)["error"]?.jsonPrimitive?.content)
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `a call with no question at all is refused`() = runTest {
        val controller = AskUserController()
        val result = AskUserToolProvider(controller).execute("ask_user", "{}", ctx)
        assertEquals("no_question", body(result)["error"]?.jsonPrimitive?.content)
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `more questions than the bar can hold is refused before any of them appears`() = runTest {
        val controller = AskUserController()
        val questions = (1..6).joinToString(",") { """{"question":"Q$it?"}""" }
        val result = AskUserToolProvider(controller)
            .execute("ask_user", """{"questions":[$questions]}""", ctx)
        assertEquals("too_many_questions", body(result)["error"]?.jsonPrimitive?.content)
        assertTrue(controller.requests.value.isEmpty())
    }

    @Test
    fun `a non-blocking set returns immediately and still delivers after the configured timeout`() = runTest {
        val controller = AskUserController()
        val delivered = mutableListOf<AskUserController.DeferredAnswer>()
        backgroundScope.launch { controller.deferredAnswers.collect { delivered += it } }
        runCurrent()
        val result = AskUserToolProvider(controller).execute(
            "ask_user",
            """{"blocking":false,"questions":[{"question":"First?"},{"question":"Second?"}]}""",
            ctx.copy(askUserTimeoutMinutes = 1),
        )
        val answer = body(result)
        assertEquals("queued", answer["delivery"]?.jsonPrimitive?.content)
        assertEquals(2, answer["questions"]!!.jsonPrimitive.int)
        assertEquals(0L, testScheduler.currentTime)
        val waiting = controller.requests.value
        assertEquals(2, waiting.size)

        advanceTimeBy(3_600_001L)
        runCurrent()
        assertEquals(waiting, controller.requests.value)
        assertTrue(delivered.isEmpty())
        controller.submit(waiting[1].id, emptyList(), "later answer")
        runCurrent()
        assertEquals("c1", delivered.single().conversationId)
        assertEquals("Second?\nlater answer", delivered.single().text)
        assertEquals(listOf(waiting[0]), controller.requests.value)
    }

    @Test
    fun `leaving blocking out waits for the answer`() = runTest {
        val controller = AskUserController()
        val call = async {
            AskUserToolProvider(controller).execute("ask_user", """{"question":"Now?"}""", ctx)
        }
        runCurrent()
        assertTrue(controller.requests.value.single().blocking)
        assertFalse(call.isCompleted)
        call.cancel()
    }

    @Test
    fun `stopping the generation cancels its group without a result or queued answer`() = runTest {
        val controller = AskUserController()
        val delivered = mutableListOf<AskUserController.DeferredAnswer>()
        backgroundScope.launch { controller.deferredAnswers.collect { delivered += it } }
        val provider = AskUserToolProvider(controller)
        val call = async {
            provider.execute(
                "ask_user",
                """{"questions":[{"question":"First?"},{"question":"Second?"}]}""",
                ctx,
            )
        }
        runCurrent()
        val waiting = controller.requests.value
        assertEquals(2, waiting.size)
        controller.submit(waiting[1].id, emptyList(), "already submitted")

        call.cancelAndJoin()
        runCurrent()

        assertTrue(call.isCancelled)
        assertTrue(controller.requests.value.isEmpty())
        waiting.forEach { assertEquals(AskUserController.Answer.Unanswered, controller.awaitAnswer(it)) }
        advanceTimeBy(300_000L)
        runCurrent()
        assertTrue(delivered.isEmpty())
    }
}
