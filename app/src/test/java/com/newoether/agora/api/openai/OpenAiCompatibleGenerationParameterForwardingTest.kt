package com.newoether.agora.api.openai

import android.content.Context
import android.content.pm.ApplicationInfo
import com.newoether.agora.api.GenerationError
import com.newoether.agora.api.LlmProvider
import com.newoether.agora.api.ProviderConfig
import com.newoether.agora.api.StreamEvent
import com.newoether.agora.model.ChatMessage
import com.newoether.agora.model.Participant
import com.newoether.agora.util.DebugLog
import com.sun.net.httpserver.HttpServer
import io.mockk.every
import io.mockk.mockk
import kotlinx.coroutines.flow.toList
import kotlinx.coroutines.runBlocking
import kotlinx.coroutines.withTimeout
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.float
import kotlinx.serialization.json.int
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import org.junit.Assert.assertEquals
import org.junit.Assert.assertFalse
import org.junit.Assert.assertTrue
import org.junit.Before
import org.junit.Test
import java.net.InetSocketAddress
import java.util.concurrent.CopyOnWriteArrayList

class OpenAiCompatibleGenerationParameterForwardingTest {
    @Before
    fun disableAndroidLoggingForJvmNetworkTests() {
        val context = mockk<Context>()
        every { context.applicationInfo } returns ApplicationInfo().apply { flags = 0 }
        DebugLog.forceEnabled = false
        DebugLog.init(context)
    }

    @Test
    fun qwenHybridForwardsToggleBudgetAndEveryApplicableChatParameter() = withServer { server ->
        val body = server.capture(
            QwenProvider(),
            config(server, "qwen-plus").copy(thinkingEnabled = false),
        )
        assertFalse(body["enable_thinking"]!!.jsonPrimitive.boolean)
        assertFalse(body.containsKey("thinking_budget"))
        assertStandardParameters(body)

        withServer { enabledServer ->
            val enabledBody = enabledServer.capture(
                QwenProvider(),
                config(enabledServer, "qwen3.6-plus").copy(
                    thinkingBudgetEnabled = true,
                    thinkingBudgetTokens = 8192,
                ),
            )
            assertTrue(enabledBody["enable_thinking"]!!.jsonPrimitive.boolean)
            assertEquals(8192, enabledBody["thinking_budget"]!!.jsonPrimitive.int)
        }
    }

    @Test
    fun qwen38UsesEffortOrBudgetButNeverBoth() = withServer { effortServer ->
        val effortBody = effortServer.capture(
            QwenProvider(),
            config(effortServer, "qwen3.8-max").copy(thinkingLevel = "high"),
        )
        assertEquals("xhigh", effortBody["reasoning_effort"]!!.jsonPrimitive.content)
        assertFalse(effortBody.containsKey("thinking_budget"))

        withServer { budgetServer ->
            val budgetBody = budgetServer.capture(
                QwenProvider(),
                config(budgetServer, "qwen3.8-flash").copy(
                    thinkingBudgetEnabled = true,
                    thinkingBudgetTokens = 16384,
                ),
            )
            assertEquals(16384, budgetBody["thinking_budget"]!!.jsonPrimitive.int)
            assertFalse(budgetBody.containsKey("reasoning_effort"))
        }
    }

    @Test
    fun qwenThinkingOnlyOffFailsBeforeHttp() = withServer { server ->
        val events = collect(
            QwenProvider(),
            config(server, "qwen3-235b-a22b-thinking-2507").copy(thinkingEnabled = false),
        )

        assertRequestFormat(events, "cannot disable thinking")
        assertTrue(server.bodies.isEmpty())
    }

    @Test
    fun groqForwardsEachDocumentedModelEffortRange() {
        assertGroqEffort("qwen/qwen3.6-27b", "xhigh", "default")
        assertGroqEffort("qwen/qwen3.8-27b", "xhigh", "high", checkParameters = true)
        assertGroqEffort("openai/gpt-oss-20b", "minimal", "low")
    }

    @Test
    fun groqGptOssOffFailsBeforeHttp() = withServer { server ->
        val events = collect(
            GroqProvider(),
            config(server, "openai/gpt-oss-120b").copy(thinkingEnabled = false),
        )

        assertRequestFormat(events, "cannot disable reasoning")
        assertTrue(server.bodies.isEmpty())
    }

    @Test
    fun customQwen38ForwardsMappedEffortAndStandardParameters() = withServer { server ->
        val body = server.capture(
            CustomOpenAiProvider("Relay", server.baseUrl),
            config(server, "qwen/qwen3.8-27b").copy(thinkingLevel = "max"),
        )
        assertEquals("xhigh", body["reasoning_effort"]!!.jsonPrimitive.content)
        assertStandardParameters(body)
    }

    private fun assertStandardParameters(body: JsonObject) {
        assertEquals(0.7f, body["temperature"]!!.jsonPrimitive.float)
        assertEquals(777, body["max_tokens"]!!.jsonPrimitive.int)
        assertEquals(0.8f, body["top_p"]!!.jsonPrimitive.float)
        assertEquals(0.2f, body["frequency_penalty"]!!.jsonPrimitive.float)
        assertEquals(-0.1f, body["presence_penalty"]!!.jsonPrimitive.float)
    }

    private fun assertRequestFormat(events: List<StreamEvent>, detail: String) {
        val error = events.filterIsInstance<StreamEvent.Error>().single().error
        assertTrue(error is GenerationError.RequestFormat)
        assertTrue((error as GenerationError.RequestFormat).details.contains(detail))
    }

    private fun assertGroqEffort(
        model: String,
        level: String,
        expected: String,
        checkParameters: Boolean = false,
    ) = withServer { server ->
        val body = server.capture(GroqProvider(), config(server, model).copy(thinkingLevel = level))
        assertEquals(expected, body["reasoning_effort"]!!.jsonPrimitive.content)
        if (checkParameters) assertStandardParameters(body)
    }

    private fun collect(provider: LlmProvider, config: ProviderConfig): List<StreamEvent> =
        runBlocking {
            withTimeout(2_000L) {
                provider.generateResponse(
                    listOf(ChatMessage(text = "hello", participant = Participant.USER)),
                    config,
                ).toList()
            }
        }

    private fun RecordingServer.capture(provider: LlmProvider, config: ProviderConfig): JsonObject {
        val events = collect(provider, config)
        assertTrue(events.none { it is StreamEvent.Error })
        return singleBody()
    }

    private fun config(server: RecordingServer, model: String) = ProviderConfig(
        apiKey = "",
        modelId = model,
        baseUrl = server.baseUrl,
        thinkingEnabled = true,
        thinkingLevel = "medium",
        temperature = 0.7f,
        maxTokens = 777,
        topP = 0.8f,
        frequencyPenalty = 0.2f,
        presencePenalty = -0.1f,
    )

    private fun withServer(test: (RecordingServer) -> Unit) {
        RecordingServer().use(test)
    }

    private class RecordingServer : AutoCloseable {
        private val server = HttpServer.create(InetSocketAddress("127.0.0.1", 0), 0)
        val bodies = CopyOnWriteArrayList<String>()
        val baseUrl = "http://127.0.0.1:${server.address.port}/v1"

        init {
            server.createContext("/") { exchange ->
                bodies += exchange.requestBody.bufferedReader().use { it.readText() }
                val response = (
                    "data: {\"choices\":[{\"index\":0,\"delta\":{},\"finish_reason\":\"stop\"}]}\n\n" +
                        "data: [DONE]\n\n"
                    ).toByteArray()
                exchange.responseHeaders.add("Content-Type", "text/event-stream")
                exchange.sendResponseHeaders(200, response.size.toLong())
                exchange.responseBody.use { it.write(response) }
            }
            server.start()
        }

        fun singleBody() = Json.parseToJsonElement(bodies.single()).jsonObject

        override fun close() = server.stop(0)
    }
}
