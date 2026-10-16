package com.newoether.agora.api.openai

import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject

/**
 * JSON acceptance rules shared by the chat and Responses wire gates: tool arguments and raw
 * protocol items pass only when they parse as exactly one JSON object. This lives in its own file
 * so the chat gate (and the golden harness that compiles it) stays self-contained.
 */
internal val validationJson = Json { ignoreUnknownKeys = true }

internal fun isJsonObject(raw: String): Boolean =
    runCatching { validationJson.parseToJsonElement(raw) is JsonObject }.getOrDefault(false)
