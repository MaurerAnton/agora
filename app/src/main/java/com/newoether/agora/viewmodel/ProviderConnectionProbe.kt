package com.newoether.agora.viewmodel

import com.newoether.agora.api.HttpClient
import com.newoether.agora.api.endpointForDisplay
import com.newoether.agora.api.extractModelFetchErrorDetail
import com.newoether.agora.api.openAiAuthHeaders
import com.newoether.agora.api.openAiEndpointCandidates
import com.newoether.agora.data.CustomEndpointProtocol
import kotlinx.coroutines.CancellationException
import kotlinx.coroutines.Dispatchers
import kotlinx.coroutines.withContext

internal const val PROBE_TIMEOUT_MILLIS = 15_000L

/** Paths a missing `/v1` segment produces; only those two move a probe to the next candidate. */
private val PATH_NOT_FOUND = setOf(404, 405)

/**
 * Wire shape a probe speaks. Custom providers bring their persisted protocol; built-in providers
 * are mapped from their class, and on-device models (no Base URL) never get a probe at all.
 */
internal enum class ProbeWire {
    OPENAI,
    ANTHROPIC,
    GOOGLE,
    OLLAMA,
}

/** One request the connection test may issue. */
internal data class ProviderProbeRequest(
    val method: String,
    val url: String,
    val headers: Map<String, String> = emptyMap(),
    val body: String? = null,
)

/**
 * One logical call and the candidate URLs it may hit, in the order a real request would try them
 * (configured literal first, then the `/v1` fallback). Candidates after a 404/405 are tried;
 * any other answer proves the path exists, so the step stops there.
 */
internal data class ProviderProbeStep(
    val candidates: List<ProviderProbeRequest>,
)

/** Observed outcome of a [ProviderProbeRequest]; `httpCode == 0` means no response arrived. */
internal data class ProviderProbeAttempt(
    val request: ProviderProbeRequest,
    val httpCode: Int,
    val elapsedMillis: Long,
    val detail: String?,
) {
    val successful: Boolean get() = httpCode in 200..299
}

/** All attempts of one connection test, in run order. */
internal data class ProviderProbeReport(
    val attempts: List<ProviderProbeAttempt>,
) {
    val successful: Boolean get() = attempts.any(ProviderProbeAttempt::successful)
}

/**
 * Builds the steps the connection test runs, using the same URL rules real traffic uses
 * (version-segment fallbacks, protocol paths, protocol auth headers), so every attempt names the
 * exact URL generation would have hit.
 *
 * The optional [chatModel] adds a one-token chat turn — the decisive check for relays whose
 * `/models` endpoint 404s on a perfectly healthy account. It costs one token.
 */
internal fun connectionProbeSteps(
    wire: ProbeWire,
    baseUrl: String,
    apiKey: String,
    chatModel: String?,
): List<ProviderProbeStep> {
    val base = baseUrl.trim().trimEnd('/')
    if (base.isEmpty()) return emptyList()

    val authHeaders = authHeaders(wire, apiKey)
    val modelList = ProviderProbeStep(
        candidates = modelListUrls(wire, base).map { url ->
            ProviderProbeRequest(method = "GET", url = url, headers = authHeaders)
        },
    )
    return listOfNotNull(modelList, chatStep(wire, base, authHeaders, chatModel))
}

private fun authHeaders(wire: ProbeWire, apiKey: String): Map<String, String> = when (wire) {
    ProbeWire.ANTHROPIC -> mapOf(
        "x-api-key" to apiKey,
        "anthropic-version" to "2023-06-01",
    )
    ProbeWire.GOOGLE -> mapOf("x-goog-api-key" to apiKey)
    ProbeWire.OPENAI, ProbeWire.OLLAMA -> openAiAuthHeaders(apiKey)
}

private fun modelListUrls(wire: ProbeWire, base: String): List<String> = when (wire) {
    ProbeWire.ANTHROPIC -> versionedCandidates(base).map { "$it/models" }
    ProbeWire.GOOGLE -> listOf(
        if (base.contains("/v1") || base.contains("/v1beta")) "$base/models" else "$base/v1beta/models",
    )
    ProbeWire.OLLAMA -> listOf("$base/api/tags")
    ProbeWire.OPENAI -> openAiEndpointCandidates(base, "models", retryMissingV1BaseUrl = true)
}

private fun chatStep(
    wire: ProbeWire,
    base: String,
    authHeaders: Map<String, String>,
    chatModel: String?,
): ProviderProbeStep? {
    val model = probeModelId(chatModel) ?: return null
    val body = when (wire) {
        ProbeWire.ANTHROPIC ->
            """{"model":"$model","max_tokens":1,"messages":[{"role":"user","content":"ping"}]}"""
        ProbeWire.OLLAMA ->
            """{"model":"$model","stream":false,"messages":[{"role":"user","content":"ping"}]}"""
        ProbeWire.OPENAI ->
            """{"model":"$model","messages":[{"role":"user","content":"ping"}],"max_tokens":1}"""
        // Gemini's generation URLs are streaming endpoints with a model baked into the path;
        // its model list already proves key and Base URL, so no chat turn is issued.
        ProbeWire.GOOGLE -> return null
    }
    val urls = when (wire) {
        ProbeWire.ANTHROPIC -> versionedCandidates(base).map { "$it/messages" }
        ProbeWire.OLLAMA -> listOf("$base/api/chat")
        ProbeWire.OPENAI ->
            openAiEndpointCandidates(base, "chat/completions", retryMissingV1BaseUrl = true)
        ProbeWire.GOOGLE -> return null
    }
    return ProviderProbeStep(
        candidates = urls.map { url ->
            ProviderProbeRequest(
                method = "POST",
                url = url,
                headers = authHeaders + ("Content-Type" to "application/json"),
                body = body,
            )
        },
    )
}

/**
 * The persisted model list carries a `provider:` prefix for custom providers; the wire takes the
 * bare id. Keeps a namespace prefix or a Gemini `models/` remnant out of the probe body.
 */
internal fun probeModelId(storedModel: String?): String? {
    if (storedModel.isNullOrBlank()) return null
    val bare = storedModel
        .substringAfter(':', missingDelimiterValue = storedModel)
        .removePrefix("models/")
    return bare.takeIf(String::isNotBlank)
}

/** Custom-provider candidates: the versioned base first, then the configured literal. */
private fun versionedCandidates(base: String): List<String> =
    customEndpointBaseUrlCandidates(CustomEndpointProtocol.ANTHROPIC, base).filterNotNull()

internal suspend fun runConnectionProbe(
    steps: List<ProviderProbeStep>,
): ProviderProbeReport = withContext(Dispatchers.IO) {
    val attempts = mutableListOf<ProviderProbeAttempt>()
    for (step in steps) {
        for ((index, request) in step.candidates.withIndex()) {
            val attempt = runProbeAttempt(request)
            attempts += attempt
            val pathMissing = attempt.httpCode in PATH_NOT_FOUND
            if (!pathMissing || index == step.candidates.lastIndex) break
        }
    }
    ProviderProbeReport(attempts)
}

private fun runProbeAttempt(request: ProviderProbeRequest): ProviderProbeAttempt {
    val startedAt = System.nanoTime()
    fun elapsedMillis() = (System.nanoTime() - startedAt) / 1_000_000L
    return try {
        val response = if (request.method == "GET") {
            HttpClient.fetchModelsResponse(request.url, request.headers, PROBE_TIMEOUT_MILLIS)
        } else {
            HttpClient.postTextResponse(
                request.url,
                request.body.orEmpty(),
                request.headers,
                PROBE_TIMEOUT_MILLIS,
            )
        }
        ProviderProbeAttempt(
            request = request,
            httpCode = response.code,
            elapsedMillis = elapsedMillis(),
            detail = if (response.isSuccessful) {
                null
            } else {
                extractModelFetchErrorDetail(response.body)
            },
        )
    } catch (error: CancellationException) {
        throw error
    } catch (error: Exception) {
        ProviderProbeAttempt(
            request = request,
            httpCode = 0,
            elapsedMillis = elapsedMillis(),
            detail = listOfNotNull(
                error.javaClass.simpleName,
                error.message?.takeIf(String::isNotBlank),
            ).joinToString(": "),
        )
    }
}

/**
 * One copy-pasteable line per attempt — `GET https://…/models → HTTP 404 · 35 ms · Not found`.
 * The query string is dropped so a credential-bearing URL can be shared from a screenshot.
 */
internal fun ProviderProbeAttempt.render(): String = buildString {
    append(request.method)
    append(' ')
    append(endpointForDisplay(request.url))
    append(" → ")
    append(if (httpCode == 0) "no response" else "HTTP $httpCode")
    append(" · ")
    append(elapsedMillis)
    append(" ms")
    detail?.takeIf(String::isNotBlank)?.let { append(" · ").append(it) }
}

internal fun ProviderProbeReport.render(): String =
    attempts.joinToString(separator = "\n") { it.render() }
