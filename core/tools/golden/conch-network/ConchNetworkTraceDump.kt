// Golden trace driver for ConchNetwork.kt and BoundedWireRead.kt.
//
// ConchNetwork.kt compiles verbatim over the stand-local OkHttp/okio subset; the trace records
// the client policy (retry/redirect/pool), the exact request ConchNetwork builds (method, URL,
// headers after case-insensitive replacement, body media type, per-call timeout) and the bounded
// response outcome (status, success, signature, body hash and the close state). BoundedWireRead.kt
// also compiles verbatim and is driven directly with scripted sources for its text/line limits.
package conch.network.golden

import com.newoether.agora.api.readBoundedWireLine
import com.newoether.agora.api.readBoundedWireText
import com.newoether.agora.util.ConchNetwork
import com.newoether.agora.util.ConchTextResponse
import java.io.File
import java.security.MessageDigest
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.contentOrNull
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.put
import okhttp3.RecordedRequest
import okhttp3.ScriptedResponse
import okhttp3.ScriptedTransport
import okio.ByteSource

private val json = Json { ignoreUnknownKeys = true }

fun main(args: Array<String>) {
    if (args.size >= 2 && args[0] == "--batch") {
        val outputDir = File(args[1])
        outputDir.mkdirs()
        for (path in args.drop(2)) {
            val scenario = File(path)
            File(outputDir, scenario.nameWithoutExtension + ".trace")
                .writeText(render(scenario) + "\n")
        }
        return
    }
    println(render(File(args[0])))
}

private fun render(scenarioFile: File): String {
    val scenario = json.parseToJsonElement(scenarioFile.readText()).jsonObject
    val op = scenario.string("op")
    return buildJsonObject {
        put("op", op)
        when (op) {
            "getTextResponse", "postTextResponse" ->
                conchTrace(op, scenario).forEach { (key, value) -> put(key, value) }
            "readBoundedWireText", "readBoundedWireLine" ->
                readTrace(op, scenario).forEach { (key, value) -> put(key, value) }
            else -> error("unknown op $op")
        }
    }.toString()
}

// ---- ConchNetwork ------------------------------------------------------------------------------

private fun conchTrace(op: String, scenario: JsonObject): JsonObject {
    ScriptedTransport.reset()
    val response = scenario["response"] as? JsonObject ?: error("missing response")
    val responseBody = response.string("body")
    val responseBytes = responseBody.toByteArray(Charsets.UTF_8)
    ScriptedTransport.response = ScriptedResponse(
        code = response.optionalInt("code") ?: 200,
        body = responseBytes,
        declaredSize = response.optionalLong("declaredSize") ?: responseBytes.size.toLong(),
        headers = response.pairs("headers"),
    )
    val headers = linkedMapOf<String, String>().apply {
        scenario.pairs("headers").forEach { (name, value) -> put(name, value) }
    }
    val url = scenario.string("url")
    val outcome = try {
        val result = if (op == "getTextResponse") {
            ConchNetwork.getTextResponse(url, headers)
        } else {
            ConchNetwork.postTextResponse(
                url,
                scenario.string("body"),
                headers,
                scenario.optionalLong("timeoutMillis"),
            )
        }
        responseJson(result)
    } catch (failure: Throwable) {
        errorJson(failure)
    }
    val request = ScriptedTransport.lastRequest
    return buildJsonObject {
        put("config", configJson())
        if (request == null) put("request", JsonNull) else put("request", requestJson(request))
        put("outcome", outcome)
        put("closed", ScriptedTransport.lastClosed)
    }
}

private fun configJson(): JsonObject = buildJsonObject {
    val client = ConchNetwork.client
    put("retryOnConnectionFailure", client.retryOnConnectionFailure)
    put("followRedirects", client.followRedirects)
    put("followSslRedirects", client.followSslRedirects)
    put("poolMaxIdle", client.connectionPool.maxIdleConnections)
    put("poolKeepAliveSeconds", client.connectionPool.keepAliveDurationMillis / 1000)
}

private fun requestJson(request: RecordedRequest): JsonObject = buildJsonObject {
    put("method", request.method)
    put("url", request.url)
    put("headers", buildJsonArray {
        request.headers.forEach { (name, value) ->
            add(buildJsonArray {
                add(name)
                add(value)
            })
        }
    })
    if (request.body == null) put("body", JsonNull) else put("body", request.body)
    if (request.contentType == null) put("contentType", JsonNull) else put("contentType", request.contentType)
    put("timeoutMillis", request.timeoutMillis)
}

private fun responseJson(response: ConchTextResponse): JsonObject {
    val bytes = response.body.toByteArray(Charsets.UTF_8)
    return buildJsonObject {
        put("code", response.code)
        put("successful", response.isSuccessful)
        put("signature", response.signature)
        put("bodyLength", bytes.size)
        put("bodySha256", sha256(bytes))
        if (response.body.length <= 64) put("bodyText", response.body)
    }
}

// ---- BoundedWireRead ----------------------------------------------------------------------------

private fun readTrace(op: String, scenario: JsonObject): JsonObject {
    val limit = scenario.optionalLong("limit") ?: error("missing limit")
    val body = scenario.string("body")
    val bytes = body.toByteArray(Charsets.UTF_8)
    val declaredSize = scenario.optionalLong("declaredSize") ?: bytes.size.toLong()
    val source = ByteSource(bytes, declaredSize)
    val outcome = try {
        if (op == "readBoundedWireText") {
            textJson(source.readBoundedWireText(limit))
        } else {
            val line = source.readBoundedWireLine(limit)
            buildJsonObject {
                if (line == null) put("line", JsonNull) else put("line", line)
            }
        }
    } catch (failure: Throwable) {
        errorJson(failure)
    }
    return buildJsonObject {
        put("limit", limit)
        put("source", buildJsonObject {
            put("bodyLength", bytes.size)
            put("declaredSize", declaredSize)
        })
        put("outcome", outcome)
    }
}

private fun textJson(text: String): JsonObject {
    val bytes = text.toByteArray(Charsets.UTF_8)
    return buildJsonObject {
        put("bodyLength", bytes.size)
        put("bodySha256", sha256(bytes))
        if (text.length <= 64) put("bodyText", text)
    }
}

private fun errorJson(failure: Throwable): JsonObject = buildJsonObject {
    put("error", buildJsonObject {
        put("type", failure::class.simpleName ?: failure::class.java.name)
        put("message", failure.message ?: "")
    })
}

private fun sha256(bytes: ByteArray): String =
    MessageDigest.getInstance("SHA-256").digest(bytes)
        .joinToString("") { String.format("%02x", it.toInt() and 0xff) }

// ---- JSON helpers -------------------------------------------------------------------------------

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String? =
    (this[name] as? JsonPrimitive)?.contentOrNull

private fun JsonObject.optionalLong(name: String): Long? = optionalString(name)?.toLong()

private fun JsonObject.optionalInt(name: String): Int? = optionalString(name)?.toInt()

private fun JsonObject.pairs(name: String): List<Pair<String, String>> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { element ->
        val pair = element as? JsonArray ?: error("$name entry is not an array")
        val key = pair.getOrNull(0) as? JsonPrimitive ?: error("$name entry has no name")
        val value = pair.getOrNull(1) as? JsonPrimitive ?: error("$name entry has no value")
        key.content to value.content
    }
}
