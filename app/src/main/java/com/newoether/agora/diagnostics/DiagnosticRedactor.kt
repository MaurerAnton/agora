package com.newoether.agora.diagnostics

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import okhttp3.HttpUrl.Companion.toHttpUrlOrNull
import java.nio.ByteBuffer
import java.nio.charset.CodingErrorAction

/** Permanently removes credentials before diagnostic payloads enter memory or durable storage. */
internal object DiagnosticRedactor {
    private const val REDACTED_SECRET = "[REDACTED_SECRET]"
    private const val REDACTED_CONTENT = "[REDACTED_CONTENT]"
    private const val INVALID_URL = "[UNAVAILABLE_INVALID_URL]"
    private const val MAX_IDENTIFIER_CHARS = 1_024
    private const val MAX_HEADERS = 128

    private val json = Json { ignoreUnknownKeys = true }

    fun captureUrl(rawUrl: String): CapturedDiagnosticText {
        val parsed = rawUrl.toHttpUrlOrNull()
        val sanitized = if (parsed == null) {
            INVALID_URL
        } else {
            val builder = parsed.newBuilder()
            if (parsed.username.isNotEmpty()) builder.username(REDACTED_SECRET)
            if (parsed.password.isNotEmpty()) builder.password(REDACTED_SECRET)
            parsed.queryParameterNames
                .filter(::isSecretKey)
                .forEach { name -> builder.setQueryParameter(name, REDACTED_SECRET) }
            redactSecrets(builder.build().toString())
        }
        return capture(rawUrl, sanitized)
    }

    fun captureHeaders(headers: Map<String, String>): Map<String, String> = buildMap {
        headers.entries.take(MAX_HEADERS).forEach { (name, value) ->
            put(
                name.take(MAX_IDENTIFIER_CHARS),
                if (isSecretKey(name)) REDACTED_SECRET else redactSecrets(value),
            )
        }
        if (headers.size > MAX_HEADERS) {
            put("[TRUNCATED_HEADERS]", (headers.size - MAX_HEADERS).toString())
        }
    }

    fun captureJson(rawJson: String): CapturedDiagnosticText {
        val (input, inputTruncated) = boundedInput(rawJson)
        val parsed = runCatching { json.parseToJsonElement(input) }.getOrNull()
        val sanitized = parsed
            ?.let(::sanitizeCredentialElement)
            ?.toString()
            ?: redactSecrets(input)
        return capture(rawJson, sanitized, alreadyTruncated = inputTruncated)
    }

    fun captureWireLine(rawLine: String): CapturedDiagnosticText {
        val (input, inputTruncated) = boundedInput(rawLine)
        val trimmed = input.trimStart()
        val leading = input.substring(0, input.length - trimmed.length)
        val sanitized = when {
            trimmed.startsWith("data:") -> {
                val data = trimmed.removePrefix("data:").trimStart()
                if (data == "[DONE]") {
                    leading + "data: [DONE]"
                } else {
                    leading + "data: " + captureJson(data).value
                }
            }
            trimmed.startsWith("{") || trimmed.startsWith("[") ->
                leading + captureJson(trimmed).value
            else -> redactSecrets(input)
        }
        return capture(rawLine, sanitized, alreadyTruncated = inputTruncated)
    }

    fun captureContent(content: String): CapturedDiagnosticText {
        val (input, inputTruncated) = boundedInput(content)
        return capture(
            original = content,
            sanitized = redactSecrets(input),
            alreadyTruncated = inputTruncated,
        )
    }

    fun redactJsonContent(captured: CapturedDiagnosticText): CapturedDiagnosticText {
        val parsed = runCatching { json.parseToJsonElement(captured.value) }.getOrNull()
        val sanitized = parsed
            ?.let { redactContentElement(it, key = null, contentScope = false) }
            ?.toString()
            ?: REDACTED_CONTENT
        return project(captured, sanitized)
    }

    fun redactWireContent(captured: CapturedDiagnosticText): CapturedDiagnosticText {
        val rawLine = captured.value
        val trimmed = rawLine.trimStart()
        val leading = rawLine.substring(0, rawLine.length - trimmed.length)
        val redacted = when {
            trimmed.startsWith("data:") -> {
                val data = trimmed.removePrefix("data:").trimStart()
                if (data == "[DONE]") {
                    leading + "data: [DONE]"
                } else {
                    val parsed = runCatching { json.parseToJsonElement(data) }.getOrNull()
                    leading + "data: " + (
                        parsed
                            ?.let { redactContentElement(it, key = null, contentScope = false) }
                            ?.toString()
                            ?: REDACTED_CONTENT
                        )
                }
            }
            trimmed.startsWith("{") || trimmed.startsWith("[") -> {
                val parsed = runCatching { json.parseToJsonElement(trimmed) }.getOrNull()
                leading + (
                    parsed
                        ?.let { redactContentElement(it, key = null, contentScope = false) }
                        ?.toString()
                        ?: REDACTED_CONTENT
                    )
            }
            isSseControl(trimmed) -> rawLine
            else -> REDACTED_CONTENT
        }
        return project(captured, redacted)
    }

    fun redactContent(captured: CapturedDiagnosticText): CapturedDiagnosticText =
        project(captured, REDACTED_CONTENT)

    fun safeIdentifier(value: String): String =
        redactSecrets(value).take(MAX_IDENTIFIER_CHARS)

    private fun sanitizeCredentialElement(
        element: JsonElement,
        key: String? = null,
    ): JsonElement {
        if (key != null && isSecretKey(key)) return JsonPrimitive(REDACTED_SECRET)
        return when (element) {
            is JsonObject -> JsonObject(
                element.mapValues { (childKey, childValue) ->
                    sanitizeCredentialElement(childValue, childKey)
                },
            )
            is JsonArray -> JsonArray(
                element.map { child -> sanitizeCredentialElement(child) },
            )
            is JsonPrimitive -> if (element.isString) {
                JsonPrimitive(redactSecrets(element.content))
            } else {
                element
            }
        }
    }

    private fun redactContentElement(
        element: JsonElement,
        key: String?,
        contentScope: Boolean,
    ): JsonElement {
        if (key != null && isSecretKey(key)) return JsonPrimitive(REDACTED_SECRET)
        val nextContentScope = contentScope ||
            (key?.normalizeKey()?.let { it in CONTENT_KEYS } == true)
        return when (element) {
            is JsonObject -> JsonObject(
                element.mapValues { (childKey, childValue) ->
                    redactContentElement(childValue, childKey, nextContentScope)
                },
            )
            is JsonArray -> JsonArray(
                element.map { child ->
                    redactContentElement(child, key = null, contentScope = nextContentScope)
                },
            )
            is JsonPrimitive -> if (element.isString) {
                JsonPrimitive(
                    if (nextContentScope) REDACTED_CONTENT else redactSecrets(element.content),
                )
            } else {
                element
            }
        }
    }

    private fun boundedInput(value: String): Pair<String, Boolean> {
        val bytes = value.toByteArray(Charsets.UTF_8)
        val oversized = bytes.size > DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES
        return if (oversized) {
            decodeUtf8Prefix(bytes, DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES) to true
        } else {
            value to false
        }
    }

    private fun capture(
        original: String,
        sanitized: String,
        alreadyTruncated: Boolean = false,
    ): CapturedDiagnosticText {
        val bytes = sanitized.toByteArray(Charsets.UTF_8)
        val oversized = bytes.size > DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES
        return CapturedDiagnosticText(
            value = if (oversized) {
                decodeUtf8Prefix(bytes, DiagnosticCaptureStore.DEFAULT_MAX_PAYLOAD_BYTES)
            } else {
                sanitized
            },
            originalLength = original.length,
            truncated = alreadyTruncated || oversized,
            redacted = true,
        )
    }

    private fun project(
        original: CapturedDiagnosticText,
        sanitized: String,
    ): CapturedDiagnosticText = capture(
        original = original.value,
        sanitized = sanitized,
        alreadyTruncated = original.truncated,
    ).copy(originalLength = original.originalLength)

    private fun isSseControl(trimmed: String): Boolean =
        trimmed.isBlank() || trimmed.startsWith(":") || trimmed.startsWith("event:") ||
            trimmed.startsWith("id:") || trimmed.startsWith("retry:")

    private fun isSecretKey(key: String): Boolean {
        val normalized = key.normalizeKey()
        return normalized in SECRET_KEYS || SECRET_KEY_SUFFIXES.any(normalized::endsWith)
    }

    private fun String.normalizeKey(): String =
        lowercase().filter(Char::isLetterOrDigit)

    private fun redactSecrets(value: String): String {
        var result = PRIVATE_KEY_BLOCK.replace(value, REDACTED_SECRET)
        result = PRIVATE_KEY_PREFIX.replace(result, REDACTED_SECRET)
        result = BEARER_SECRET.replace(result) { match ->
            match.groupValues[1] + REDACTED_SECRET
        }
        result = NAMED_SECRET.replace(result) { match ->
            match.groupValues[1] + match.groupValues[2] + REDACTED_SECRET
        }
        SECRET_TOKEN_PATTERNS.forEach { pattern ->
            result = pattern.replace(result, REDACTED_SECRET)
        }
        return result
    }

    private fun decodeUtf8Prefix(bytes: ByteArray, maxBytes: Int): String =
        Charsets.UTF_8.newDecoder()
            .onMalformedInput(CodingErrorAction.IGNORE)
            .onUnmappableCharacter(CodingErrorAction.IGNORE)
            .decode(ByteBuffer.wrap(bytes, 0, maxBytes))
            .toString()

    private val SECRET_KEYS = setOf(
        "authorization",
        "proxyauthorization",
        "apikey",
        "xapikey",
        "xgoogapikey",
        "cookie",
        "setcookie",
        "key",
        "password",
        "passwd",
        "secret",
        "clientsecret",
        "accesstoken",
        "refreshtoken",
        "securitytoken",
        "sessiontoken",
        "token",
        "signature",
        "sig",
        "credential",
        "xamzcredential",
        "xamzsignature",
        "xamzsecuritytoken",
        "xgoogcredential",
        "xgoogsignature",
        "googleaccessid",
        "awsaccesskeyid",
        "proxyusername",
        "proxypassword",
    )
    private val SECRET_KEY_SUFFIXES = setOf(
        "apikey",
        "accesstoken",
        "refreshtoken",
        "clientsecret",
        "securitytoken",
        "sessiontoken",
        "signature",
        "credential",
    )
    private val CONTENT_KEYS = setOf(
        "arguments",
        "content",
        "input",
        "output",
        "prompt",
        "query",
        "reasoning",
        "reasoningcontent",
        "result",
        "systeminstruction",
        "text",
        "thinking",
        "thought",
        "toolarguments",
        "toolresult",
    )
    private val BEARER_SECRET = Regex(
        """(?i)\b(Bearer\s+)[A-Za-z0-9._~+/=-]+""",
    )
    private val NAMED_SECRET = Regex(
        """(?i)\b(api[_-]?key|access[_-]?token|refresh[_-]?token|security[_-]?token|session[_-]?token|authorization|proxy[_-]?authorization|cookie|password|secret|signature|credential|token)\s*([=:])\s*["']?[^\s"'&,}]+""",
    )
    private val PRIVATE_KEY_BLOCK = Regex(
        """-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----.*?-----END [A-Z0-9 ]*PRIVATE KEY-----""",
        setOf(RegexOption.IGNORE_CASE, RegexOption.DOT_MATCHES_ALL),
    )
    private val PRIVATE_KEY_PREFIX = Regex(
        """-----BEGIN [A-Z0-9 ]*PRIVATE KEY-----.*""",
        setOf(RegexOption.IGNORE_CASE, RegexOption.DOT_MATCHES_ALL),
    )
    private val SECRET_TOKEN_PATTERNS = listOf(
        Regex("""\bsk-[A-Za-z0-9_-]{12,}\b"""),
        Regex("""\bAIza[A-Za-z0-9_-]{20,}\b"""),
        Regex("""\bgh[pousr]_[A-Za-z0-9]{20,}\b"""),
        Regex("""\bxox[baprs]-[A-Za-z0-9-]{12,}\b"""),
        Regex("""\beyJ[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\.[A-Za-z0-9_-]+\b"""),
    )
}
