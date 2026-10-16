package com.newoether.agora.api.openai

import com.newoether.agora.api.*

import com.newoether.agora.util.DebugLog
import com.newoether.agora.api.util.Base64FileRegistry
import com.newoether.agora.api.util.convertToOpenAiMessages
import com.newoether.agora.api.util.toRequestBody
import com.newoether.agora.api.util.prepareMessages
import com.newoether.agora.api.util.RequestFormatException
import com.newoether.agora.api.util.requireValidSerializedRequest
import com.newoether.agora.api.util.StreamTermination
import com.newoether.agora.api.util.asRetryableResponseBodyReadError
import com.newoether.agora.api.util.asRetryableTransportError
import com.newoether.agora.api.util.carriesModelOutput
import com.newoether.agora.api.util.ProviderRetryPolicy
import com.newoether.agora.model.ChatMessage
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.delay
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.currentCoroutineContext
import kotlinx.coroutines.flow.Flow
import kotlinx.coroutines.flow.flow
import kotlinx.coroutines.flow.flowOn
import kotlinx.coroutines.isActive
import kotlinx.coroutines.withContext
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonPrimitive
import java.net.ConnectException
import java.net.SocketTimeoutException
import java.net.UnknownHostException

abstract class BaseOpenAiProvider : LlmProvider {

    protected val json = Json { ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false }

    // -- Override points --

    /**
     * Modify the outgoing request before serialization (e.g. add reasoning_effort, plugins).
     * The default implementation returns the request unchanged.
     */
    protected open fun customizeRequest(request: OpenAiChatRequest, config: ProviderConfig): OpenAiChatRequest = request

    /**
     * Extra HTTP headers to include in the POST to /chat/completions.
     */
    protected open fun getExtraHeaders(config: ProviderConfig): Map<String, String> = emptyMap()

    /**
     * Transform the system prompt before it is sent. Default: pass-through.
     */
    protected open fun transformSystemPrompt(prompt: String?): String? = prompt

    /**
     * Replay each assistant turn's stored chain of thought as `reasoning_content`. DeepSeek
     * thinking mode answers a tools request with 400 unless every earlier turn carries it; other
     * providers ignore the field, so the default is off.
     */
    protected open fun forwardsAssistantReasoningContent(config: ProviderConfig): Boolean = false

    /**
     * Parse one OpenAI-compatible delta into native thought and raw answer events.
     * Provider-neutral inline marker recovery is owned by ProviderStreamNormalizer.
     */
    protected open suspend fun parseDeltaContent(
        delta: OpenAiDelta,
        config: ProviderConfig,
        emit: suspend (StreamEvent) -> Unit
    ) {
        defaultOpenAiDeltaContent(delta, emit)
    }

    /**
     * True while [parseDeltaContent] is the base implementation, which is the only semantics the
     * native consumer implements. A provider with its own [parseDeltaContent] overrides this to
     * false and keeps the Kotlin reference consumer.
     */
    protected open val usesDefaultDeltaParsing: Boolean get() = true

    /**
     * One stream consumer per attempt: the native port when it is available and valid, the Kotlin
     * reference otherwise. Both decide per-line meaning identically, pinned by the golden traces
     * under `core/tools/golden/openai`.
     */
    internal open fun createChatStreamConsumer(config: ProviderConfig): OpenAiChatStreamConsumption {
        if (usesDefaultDeltaParsing) {
            NativeOpenAiChatStreamConsumer.createOrNull(
                providerName = name,
                logMalformed = { DebugLog.e("AgoraAPI", it) },
            )?.let { return it }
        }
        return OpenAiChatStreamConsumer(
            json = json,
            providerName = name,
            parseDelta = { delta, emit -> parseDeltaContent(delta, config, emit) },
            logMalformed = { DebugLog.e("AgoraAPI", it) },
        )
    }

    protected open val retryableStatusCodes: Set<Int> = setOf(429, 502, 503, 504)

    protected open val retryMissingV1BaseUrl: Boolean = false

    /**
     * OpenAI normally follows `finish_reason` with an optional usage-only event and `[DONE]`.
     * Compatible gateways sometimes omit `[DONE]` or keep the HTTP connection alive. Once the
     * semantic terminal event arrives, accept that tail only for this bounded window.
     */
    protected open val terminalSseGraceMillis: Long = 1_000L

    protected open fun retryDelayMillis(attempt: Int): Long =
        ProviderRetryPolicy.delayMillis(attempt)

    // -- Template method --

    override fun generateResponse(
        messages: List<ChatMessage>,
        config: ProviderConfig
    ): Flow<StreamEvent> = flow {
        val baseUrl = config.baseUrl?.trimEnd('/')?.ifBlank { null } ?: defaultBaseUrl
        val endpointUrls = openAiEndpointCandidates(
            baseUrl = baseUrl,
            path = if (config.responsesApiEnabled) "responses" else "chat/completions",
            retryMissingV1BaseUrl = retryMissingV1BaseUrl,
        )
        try {
            if (config.openAiWebSearchEnabled && !config.responsesApiEnabled) {
                throw RequestFormatException(
                    name,
                    listOf("hosted web search requires Responses API transport"),
                )
            }
            val headers = mutableMapOf("Content-Type" to "application/json")
            if (config.apiKey.isNotBlank()) headers["Authorization"] = "Bearer ${config.apiKey}"
            for ((key, value) in getExtraHeaders(config)) headers[key] = value

            val maxAttempts = ProviderRetryPolicy.MAX_ATTEMPTS
            var attempt = 0
            var finished = false

            while (attempt < maxAttempts && !finished) {
                attempt++
                var endpointIndex = 0
                var retryScheduled = false

                while (endpointIndex < endpointUrls.size && !finished && !retryScheduled) {
                    val endpointUrl = endpointUrls[endpointIndex]
                    val resolvedRequest = config.resolveRequest(messages)
                    val built = if (config.responsesApiEnabled) {
                        buildResponsesRequest(config, resolvedRequest)
                    } else {
                        chatRequestFactory.build(config, resolvedRequest)
                    }
                    DebugLog.d(
                        "AgoraAPI",
                        "[$name] request transport=" +
                            "${if (config.responsesApiEnabled) "responses" else "chat"} " +
                            "model=${config.modelId} messages=${built.apiMessages.size} " +
                            "tools=${config.tools?.size ?: 0}",
                    )
                    // Opening the request can fail before any response headers exist (connect
                    // timeout, TLS failure, reset). Those escaped the retry loop entirely before,
                    // so a single flaky connection became a hard failure. Nothing has streamed at
                    // this point, so replaying is always safe.
                    val handle = try {
                        if (built.streamingBody != null) {
                            HttpClient.streamPostBody(
                                endpointUrl,
                                built.streamingBody.toRequestBody(),
                                headers,
                                built.streamingBody.diagnosticJson,
                            )
                        } else {
                            HttpClient.streamPost(endpointUrl, built.bodyJson, headers)
                        }
                    } catch (e: CancellationException) {
                        throw e
                    } catch (e: Exception) {
                        val retryable = e.asRetryableTransportError()
                        if (retryable != null && attempt < maxAttempts) {
                            val retryDelayMs = retryDelayMillis(attempt)
                            DebugLog.w(
                                "AgoraAPI",
                                "[$name] Transport failure opening stream on attempt " +
                                    "$attempt/$maxAttempts (${e.javaClass.simpleName}), " +
                                    "retrying in ${retryDelayMs}ms",
                            )
                            emit(StreamEvent.Retrying(attempt, ProviderRetryPolicy.MAX_RETRIES))
                            delay(retryDelayMs)
                            retryScheduled = true
                            continue
                        }
                        throw e
                    }
                    try {
                        if (handle.code == 200) {
                            // HTTP 200 only proves the request was accepted. Whether the MESSAGE
                            // completed is decided from semantic markers below, so a relay that
                            // cuts the stream at a content-block boundary can no longer pass as a
                            // finished answer.
                            val termination = if (config.responsesApiEnabled) {
                                consumeResponsesStream(handle, config) { emit(it) }
                            } else {
                                consumeSuccessfulStream(handle, config) { emit(it) }
                            }
                            DebugLog.d(
                                "AgoraSSE",
                                "[$name] stream_end ${termination.describe()} " +
                                    "attempt=$attempt/$maxAttempts",
                            )
                            if (termination.isRetryable && attempt < maxAttempts) {
                                // Nothing was surfaced yet, so a replay cannot duplicate output.
                                DebugLog.w(
                                    "AgoraAPI",
                                    "[$name] Incomplete stream on attempt $attempt/$maxAttempts, retrying",
                                )
                                val retryDelayMs = retryDelayMillis(attempt)
                                emit(StreamEvent.Retrying(attempt, ProviderRetryPolicy.MAX_RETRIES))
                                delay(retryDelayMs)
                                retryScheduled = true
                            } else {
                                termination.toError(name)?.let { emit(StreamEvent.Error(it)) }
                                finished = true
                            }
                        } else {
                            val errorRaw = handle.errorBody.orEmpty()
                            val hasV1Fallback = endpointIndex + 1 < endpointUrls.size
                            if (hasV1Fallback) {
                                DebugLog.w(
                                    "AgoraAPI",
                                    "[$name] HTTP ${handle.code}; trying endpoint candidate " +
                                        "${endpointIndex + 2}/${endpointUrls.size}",
                                )
                                endpointIndex++
                                continue
                            }

                            val responseBytes = errorRaw.toByteArray(Charsets.UTF_8).size
                            DebugLog.e(
                                "AgoraAPI",
                                "[$name] HTTP ${handle.code} responseBytes=$responseBytes",
                            )

                            if (
                                ProviderRetryPolicy.shouldRetryHttp(
                                    handle.code,
                                    errorRaw,
                                    retryableStatusCodes,
                                ) && attempt < maxAttempts
                            ) {
                                val retryDelayMs = retryDelayMillis(attempt)
                                DebugLog.w("AgoraAPI", "[$name] Transient error ${handle.code} on attempt $attempt/$maxAttempts, retrying in ${retryDelayMs}ms...")
                                emit(StreamEvent.Retrying(attempt, ProviderRetryPolicy.MAX_RETRIES))
                                delay(retryDelayMs)
                                retryScheduled = true
                            } else {
                                emit(StreamEvent.Error(buildGenerationError(handle.code, errorRaw, endpointUrls)))
                                finished = true
                            }
                        }
                    } finally {
                        handle.close()
                    }
                }
            }
        } catch (e: CancellationException) {
            throw e
        } catch (e: RequestFormatException) {
            DebugLog.e("AgoraAPI", "[$name] blocked invalid request: ${e.violations.joinToString()}")
            emit(StreamEvent.Error(GenerationError.RequestFormat(name, e.violations.joinToString())))
        } catch (e: SocketTimeoutException) {
            emit(StreamEvent.Error(GenerationError.Timeout))
        } catch (e: ConnectException) {
            emit(StreamEvent.Error(GenerationError.Network(statusCode = 0, message = e.localizedMessage ?: "Connection refused")))
        } catch (e: UnknownHostException) {
            emit(StreamEvent.Error(GenerationError.Network(statusCode = 0, message = e.localizedMessage ?: "Unknown host")))
        } catch (e: Exception) {
            if (currentCoroutineContext().isActive) {
                emit(StreamEvent.Error(GenerationError.Unknown(e)))
            }
        }
    }.flowOn(Dispatchers.IO)

    private suspend fun consumeResponsesStream(
        handle: HttpClient.StreamHandle,
        config: ProviderConfig,
        emit: suspend (StreamEvent) -> Unit,
    ): StreamTermination {
        val router = OpenAiResponsesEventRouter(json)
        var producedContent = false
        var reportedError = false
        var streamError: GenerationError? = null
        var responseBodyReadError: GenerationError? = null
        var timedOut = false
        var consecutiveReadTimeouts = 0

        while (currentCoroutineContext().isActive) {
            val line = try {
                handle.readLine()
            } catch (error: SocketTimeoutException) {
                if (!currentCoroutineContext().isActive) break
                if (++consecutiveReadTimeouts >= 3) {
                    timedOut = true
                    break
                }
                continue
            } catch (error: Exception) {
                if (!currentCoroutineContext().isActive) break
                responseBodyReadError = error.asRetryableResponseBodyReadError() ?: throw error
                break
            } ?: break
            consecutiveReadTimeouts = 0
            if (!line.startsWith("data: ")) continue
            val payload = line.substring(6).trim()
            if (payload == "[DONE]") break

            val routed = try {
                router.route(json.decodeFromString<OpenAiResponseStreamEvent>(payload))
            } catch (error: Exception) {
                DebugLog.e(
                    "AgoraAPI",
                    "[$name] malformed Responses payload exception=${error.javaClass.simpleName}",
                )
                streamError = GenerationError.SseParse(
                    rawLine = payload.take(512),
                    cause = error.localizedMessage ?: "Malformed Responses SSE payload",
                )
                emptyList()
            }
            routed.forEach { event ->
                if (event is StreamEvent.Error && event.error is GenerationError.SseParse) {
                    streamError = event.error
                } else {
                    if (event.carriesModelOutput()) producedContent = true
                    if (event is StreamEvent.Error) reportedError = true
                    emit(event)
                }
            }
            if (
                streamError != null || reportedError || router.streamError != null ||
                router.sawTerminalMarker
            ) break
        }

        if (!currentCoroutineContext().isActive) {
            throw CancellationException("Responses stream cancelled")
        }
        return StreamTermination(
            sawTerminalMarker = router.sawTerminalMarker,
            stopReason = router.stopReason,
            producedContent = producedContent,
            toolCallInFlight = router.toolCallInFlight,
            streamError = responseBodyReadError ?: streamError ?: router.streamError,
            alreadyReportedError = reportedError,
            timedOut = timedOut,
            retryableStreamError = responseBodyReadError != null || !router.sawTerminalMarker,
        )
    }

    /**
     * Consume one 200 SSE stream and report HOW it ended.
     *
     * The returned [StreamTermination] is the only evidence that the message actually completed:
     * socket EOF is produced identically by a clean finish and by a relay cutting the connection
     * between content blocks. Fatal errors the consumer itself raises are flagged so the caller
     * does not report a second, contradictory diagnostic.
     *
     * The provider owns the socket, the read-timeout budget and the terminal grace deadline; the
     * per-line meaning is owned by [OpenAiChatStreamConsumption].
     */
    private suspend fun consumeSuccessfulStream(
        handle: HttpClient.StreamHandle,
        config: ProviderConfig,
        emit: suspend (StreamEvent) -> Unit
    ): StreamTermination {
        val consumer = createChatStreamConsumer(config)
        // The native consumer holds a handle for the whole attempt; release it on every path,
        // cancellation included. The Kotlin reference needs no cleanup.
        try {
            var terminalDeadlineNanos: Long? = null

            // Read timeouts are tolerated for long thinking pauses (read timeout = 5 min), but a
            // silently-dead connection (NAT drop with no RST) must not hang forever: give up after
            // 3 consecutive timeouts (~15 min without a single byte).
            while (currentCoroutineContext().isActive) {
                terminalDeadlineNanos?.let { deadline ->
                    val remainingNanos = deadline - System.nanoTime()
                    if (remainingNanos <= 0L) break
                    val remainingMillis =
                        java.util.concurrent.TimeUnit.NANOSECONDS.toMillis(remainingNanos)
                            .coerceAtLeast(1L)
                    handle.setReadTimeoutMillis(remainingMillis)
                }
                val line = try {
                    handle.readLine()
                } catch (e: SocketTimeoutException) {
                    if (!currentCoroutineContext().isActive) break
                    // A semantic terminal event already completed the model response. This timeout
                    // only closes its optional usage/[DONE] grace tail and is therefore success.
                    if (!consumer.onReadTimeout(terminalDeadlineNanos != null)) break
                    continue
                } catch (e: Exception) {
                    if (!currentCoroutineContext().isActive) break
                    val mapped = e.asRetryableResponseBodyReadError() ?: throw e
                    consumer.onReadError(mapped)
                    break
                } ?: break

                if (!consumer.consumeLine(line, emit)) break

                if (terminalDeadlineNanos == null && consumer.hasSemanticTerminal) {
                    terminalDeadlineNanos =
                        System.nanoTime() +
                            java.util.concurrent.TimeUnit.MILLISECONDS.toNanos(
                                terminalSseGraceMillis.coerceAtLeast(1L)
                            )
                }
            }

            // Do not promote an open structured call at transport EOF. Only [DONE] or finish_reason
            // can prove that its metadata is complete; leaving it pending makes the termination
            // error carry toolCallInFlight=true and prevents execution of a truncated invocation.

            if (!currentCoroutineContext().isActive) {
                throw CancellationException("Stream cancelled")
            }

            return consumer.finish()
        } finally {
            (consumer as? AutoCloseable)?.close()
        }
    }

    private val chatRequestFactory: OpenAiChatRequestFactory by lazy {
        OpenAiChatRequestFactory(
            providerName = name,
            json = json,
            customizeRequest = { request, config -> customizeRequest(request, config) },
            transformSystemPrompt = { transformSystemPrompt(it) },
            forwardsAssistantReasoning = { forwardsAssistantReasoningContent(it) },
        )
    }

    /**
     * The Responses-transport body keeps its own builder: it is a separate wire protocol and the
     * chat-completions slice does not cover it.
     */
    private fun buildResponsesRequest(
        config: ProviderConfig,
        requestInput: ProviderRequestInput,
    ): OpenAiBuiltRequest {
        val base64Files = Base64FileRegistry()
        val apiMessages = convertToOpenAiMessages(
            messages = requestInput.messages,
            systemPrompt = transformSystemPrompt(requestInput.systemPrompt),
            includeImages = config.includeImages,
            base64Files = base64Files,
            forwardAssistantReasoning = forwardsAssistantReasoningContent(config),
        )
        val request = OpenAiResponsesRequest(
            model = config.modelId,
            input = apiMessages.toResponsesInput(
                providerName = name,
                targetModel = config.modelId,
            ),
            tools = buildList {
                addAll(config.tools.orEmpty().toResponsesTools())
                if (config.openAiWebSearchEnabled) {
                    add(OpenAiResponseTool(type = "web_search"))
                }
            }.ifEmpty { null },
            reasoning = config.thinkingLevel.takeIf { config.thinkingEnabled }
                ?.let {
                    OpenAiReasoning(
                        effort = it,
                        summary = "auto",
                    )
                },
            serviceTier = config.openAiServiceTier,
            temperature = config.temperature,
            maxOutputTokens = config.maxTokens,
            topP = config.topP,
            promptCacheKey = config.promptCacheKey,
        )
        request.requireValidWireFormat(name)
        val bodyJson = json.encodeToString(OpenAiResponsesRequest.serializer(), request)
        requireValidSerializedRequest(
            provider = name,
            body = bodyJson,
            requiredStringFields = setOf("model"),
            requiredArrayFields = setOf("input"),
        )
        return OpenAiBuiltRequest(
            apiMessages = apiMessages,
            bodyJson = bodyJson,
            streamingBody = base64Files.prepare(bodyJson),
        )
    }

    private fun buildGenerationError(
        statusCode: Int,
        errorRaw: String,
        endpointUrls: List<String>
    ): GenerationError {
        val endpointHint = if (statusCode == 404 && endpointUrls.size > 1) {
            "\nTried ${endpointUrls.joinToString(" and ")}. OpenAI-compatible servers often require a /v1 Base URL."
        } else {
            ""
        }
        val error = providerHttpError(statusCode, errorRaw)
        return if (endpointHint.isEmpty()) {
            error
        } else {
            error.copy(message = error.message + endpointHint)
        }
    }

    private fun fetchModelPages(
        endpointUrl: String,
        headers: Map<String, String>,
    ): List<String> {
        val modelIds = linkedSetOf<String>()
        val seenCursors = mutableSetOf<String>()
        var pageUrl = endpointUrl

        repeat(MAX_MODEL_LIST_PAGES) { pageIndex ->
            val page = try {
                val responseText = HttpClient.fetchModelsResponse(pageUrl, headers)
                    .requireModelFetchBody()
                decodeModelFetchResponse {
                    json.decodeFromString<OpenAiModelListResponse>(responseText)
                }
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                if (pageIndex == 0) throw error
                DebugLog.w(
                    "AgoraAPI",
                    "Stopped paginating $name models after $pageIndex completed pages; " +
                        "returning ${modelIds.size} models",
                )
                return modelIds.sorted()
            }

            modelIds += page.data.map { it.id }
            if (!page.hasMore) return modelIds.sorted()

            val cursor = page.lastId?.takeIf(String::isNotBlank)
                ?: page.data.lastOrNull()?.id?.takeIf(String::isNotBlank)
            if (cursor == null) {
                DebugLog.w(
                    "AgoraAPI",
                    "$name model list reported has_more without a usable cursor; " +
                        "returning ${modelIds.size} models",
                )
                return modelIds.sorted()
            }
            if (!seenCursors.add(cursor)) {
                DebugLog.w(
                    "AgoraAPI",
                    "$name model list repeated a cursor; " +
                        "returning ${modelIds.size} models",
                )
                return modelIds.sorted()
            }

            pageUrl = nextOpenAiModelPageUrl(endpointUrl, cursor)
        }

        DebugLog.w(
            "AgoraAPI",
            "$name model list exceeded $MAX_MODEL_LIST_PAGES pages; " +
                "returning ${modelIds.size} models",
        )
        return modelIds.sorted()
    }

    override suspend fun fetchModels(apiKey: String, baseUrl: String?): List<String> = withContext(Dispatchers.IO) {
        val effectiveBaseUrl = baseUrl?.trimEnd('/')?.ifBlank { null } ?: defaultBaseUrl
        val endpointUrls = openAiEndpointCandidates(
            effectiveBaseUrl,
            "models",
            retryMissingV1BaseUrl,
        )
        val headers = openAiAuthHeaders(apiKey)
        var lastFailure: Exception? = null

        for ((index, endpointUrl) in endpointUrls.withIndex()) {
            try {
                val models = fetchModelPages(endpointUrl, headers)
                if (models.isEmpty()) throw ModelFetchEmptyResultException()
                return@withContext models
            } catch (error: CancellationException) {
                throw error
            } catch (error: Exception) {
                lastFailure = error
                if (index < endpointUrls.lastIndex) {
                    DebugLog.w(
                        "AgoraAPI",
                        "Failed to fetch $name models; trying endpoint candidate " +
                            "${index + 2}/${endpointUrls.size}",
                    )
                }
            }
        }

        val failure = lastFailure ?: ModelFetchEmptyResultException()
        DebugLog.e(
            "AgoraAPI",
            "Failed to fetch $name models exception=${failure.javaClass.simpleName}",
        )
        throw failure
    }

    private companion object {
        const val MAX_MODEL_LIST_PAGES = 50
    }
}
