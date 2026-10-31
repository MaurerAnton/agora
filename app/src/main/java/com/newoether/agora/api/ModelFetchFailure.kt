package com.newoether.agora.api

import com.newoether.agora.util.DebugLog
import kotlinx.coroutines.CancellationException
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.contentOrNull
import java.io.IOException
import okhttp3.HttpUrl.Companion.toHttpUrl

internal class ModelFetchHttpException(
    val statusCode: Int,
    val detail: String?,
    val endpoint: String? = null,
) : IOException(
    buildString {
        append("HTTP ").append(statusCode)
        detail?.takeIf(String::isNotBlank)?.let { append(" — ").append(it) }
        endpoint?.let(::appendEndpointContext)
    }
)

internal class ModelFetchEmptyResultException : IOException("No models returned")

internal class ModelFetchTimeoutException : IOException("Request timed out")

internal class ModelFetchInvalidResponseException(
    internal val source: Throwable,
    val endpoint: String? = null,
) : IOException(
    source.message?.let { message -> message + endpointContext(endpoint) } ?: source.message,
    source,
)

private fun endpointContext(endpoint: String?): String =
    endpoint
        ?.let(::endpointForDisplay)
        ?.takeIf(String::isNotEmpty)
        ?.let { " ($it)" }
        .orEmpty()

private fun StringBuilder.appendEndpointContext(endpoint: String) {
    endpointForDisplay(endpoint)
        .takeIf(String::isNotEmpty)
        ?.let { append(" (").append(it).append(')') }
}

/**
 * Attaches the endpoint a model-list failure came from while the attempt still knows which URL
 * was used. Without it the user sees `HTTP 404 — Not found` and has no way to tell a wrong Base
 * URL from a broken key. Failures that already carry an endpoint keep it.
 */
internal fun Exception.withModelFetchEndpoint(endpoint: String?): Exception = when {
    endpoint.isNullOrBlank() -> this
    this is ModelFetchHttpException && this.endpoint == null ->
        ModelFetchHttpException(statusCode, detail, endpoint)
    this is ModelFetchInvalidResponseException && this.endpoint == null ->
        ModelFetchInvalidResponseException(source, endpoint)
    else -> this
}

private val modelFetchErrorJson = Json { ignoreUnknownKeys = true }
private val modelFetchWhitespace = Regex("\\s+")

internal fun normalizeModelFetchDetail(
    value: String,
    maxLength: Int = 240,
): String {
    require(maxLength >= 2)
    val normalized = modelFetchWhitespace.replace(
        value.filterNot { char -> char.isISOControl() && !char.isWhitespace() },
        " ",
    ).trim()
    return if (normalized.length <= maxLength) {
        normalized
    } else {
        normalized.take(maxLength - 1).trimEnd() + "…"
    }
}

internal fun extractModelFetchErrorDetail(body: String): String? {
    val normalizedBody = normalizeModelFetchDetail(body)
    if (normalizedBody.isBlank()) return null
    val parsed = runCatching { modelFetchErrorJson.parseToJsonElement(body) }.getOrNull()
    val structured = parsed?.modelFetchErrorDetail()
    return normalizeModelFetchDetail(structured ?: normalizedBody).takeIf(String::isNotBlank)
}

internal fun HttpClient.TextResponse.requireModelFetchBody(): String {
    if (!isSuccessful) {
        throw ModelFetchHttpException(
            statusCode = code,
            detail = extractModelFetchErrorDetail(body),
        )
    }
    if (body.isBlank()) {
        throw ModelFetchInvalidResponseException(IOException("Empty response body"))
    }
    return body
}

internal inline fun <T> decodeModelFetchResponse(block: () -> T): T = try {
    block()
} catch (error: CancellationException) {
    throw error
} catch (error: ModelFetchHttpException) {
    throw error
} catch (error: Exception) {
    throw ModelFetchInvalidResponseException(error)
}

private fun JsonElement.modelFetchErrorDetail(): String? = when (this) {
    is JsonPrimitive -> contentOrNull
    is JsonObject -> {
        sequenceOf("message", "detail", "error_description")
            .mapNotNull { key -> this[key]?.modelFetchErrorDetail() }
            .firstOrNull(String::isNotBlank)
            ?: this["error"]?.modelFetchErrorDetail()
    }
    else -> null
}

internal fun openAiEndpointCandidates(
    baseUrl: String,
    path: String,
    retryMissingV1BaseUrl: Boolean,
): List<String> {
    val normalizedBaseUrl = baseUrl.trimEnd('/')
    val cleanPath = path.trimStart('/')
    val primary = "$normalizedBaseUrl/$cleanPath"
    if (!retryMissingV1BaseUrl || normalizedBaseUrl.isBlank() ||
        BaseUrlResolver.hasVersionSegment(normalizedBaseUrl)
    ) {
        return listOf(primary)
    }
    return listOf(primary, "$normalizedBaseUrl/v1/$cleanPath")
}

internal fun openAiAuthHeaders(apiKey: String): Map<String, String> =
    if (apiKey.isBlank()) emptyMap() else mapOf("Authorization" to "Bearer $apiKey")

internal fun nextOpenAiModelPageUrl(endpointUrl: String, cursor: String): String =
    endpointUrl.toHttpUrl()
        .newBuilder()
        .addQueryParameter("after", cursor)
        .build()
        .toString()
