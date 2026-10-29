// Golden trace driver for the transport-neutral policy extracted from HttpClient.kt.
//
// HttpClient.kt is a 608-line OkHttp/Android transport object. `agora_extract http-policy` copies the pure
// policy declarations out of it verbatim (visibility widened, dedented) and the stand exercises
// host locality, the cleartext credential guard, proxy sanitisation, bypass/CIDR matching,
// proxy authorization and the base client policy.
package http.policy.golden

import com.newoether.agora.api.ClientPolicy
import com.newoether.agora.api.ProxyConfig
import com.newoether.agora.api.ProxyType
import com.newoether.agora.api.clientPolicy
import com.newoether.agora.api.guardCleartextCredentials
import com.newoether.agora.api.ipv4InCidr
import com.newoether.agora.api.isLocalHost
import com.newoether.agora.api.isProxyBypassed
import com.newoether.agora.api.proxyAuthorizationHeader
import com.newoether.agora.api.resolveProxy
import com.newoether.agora.api.sanitizedProxyConfig
import java.io.File
import java.net.InetSocketAddress
import java.net.Proxy
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonElement
import kotlinx.serialization.json.JsonNull
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.add
import kotlinx.serialization.json.buildJsonArray
import kotlinx.serialization.json.buildJsonObject
import kotlinx.serialization.json.jsonArray
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import kotlinx.serialization.json.put

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
            "isLocalHost" -> {
                val host = scenario.string("host")
                put("host", host)
                put("outcome", buildJsonObject { put("local", isLocalHost(host)) })
            }
            "guardCleartextCredentials" -> {
                val url = scenario.string("url")
                val headers = scenario.pairs("headers")
                put("url", url)
                put("headers", headersJson(headers))
                put("outcome", guardOutcome(url, headers))
            }
            "sanitizeProxy" -> {
                val config = scenario.proxyConfig("config")
                put("config", configJson(config))
                put("outcome", buildJsonObject {
                    put("config", configJson(sanitizedProxyConfig(config)))
                })
            }
            "resolveProxy" -> {
                val host = scenario.string("host")
                val config = scenario.proxyConfig("config")
                put("host", host)
                put("config", configJson(config))
                put("outcome", buildJsonObject {
                    put("proxy", proxyJson(resolveProxy(host, sanitizedProxyConfig(config))))
                })
            }
            "isProxyBypassed" -> {
                val host = scenario.string("host")
                val bypass = scenario.strings("bypass")
                put("host", host)
                put("bypass", stringsJson(bypass))
                put("outcome", buildJsonObject { put("bypassed", isProxyBypassed(host, bypass)) })
            }
            "ipv4InCidr" -> {
                val host = scenario.string("host")
                val cidr = scenario.string("cidr")
                put("host", host)
                put("cidr", cidr)
                put("outcome", buildJsonObject { put("match", ipv4InCidr(host, cidr)) })
            }
            "proxyAuthorization" -> {
                val config = scenario.proxyConfig("config")
                val existing = scenario.boolean("hasExistingHeader")
                put("config", configJson(config))
                put("hasExistingHeader", existing)
                put("outcome", buildJsonObject {
                    put("header", proxyAuthorizationHeader(sanitizedProxyConfig(config), existing))
                })
            }
            "clientPolicy" -> put("outcome", policyJson(clientPolicy()))
            else -> error("unknown op $op")
        }
    }.toString()
}

private fun guardOutcome(
    url: String,
    headers: List<Pair<String, String>>,
): JsonObject {
    val map = linkedMapOf<String, String>()
    for ((name, value) in headers) map[name] = value
    return try {
        guardCleartextCredentials(url, map)
        buildJsonObject { put("ok", true) }
    } catch (failure: Throwable) {
        errorJson(failure)
    }
}

private fun errorJson(failure: Throwable): JsonObject = buildJsonObject {
    put("error", buildJsonObject {
        put("type", failure.javaClass.simpleName)
        put("message", failure.message ?: "")
    })
}

private fun configJson(config: ProxyConfig?): JsonElement {
    if (config == null) return JsonNull
    return buildJsonObject {
        put("type", config.type.name)
        put("host", config.host)
        put("port", config.port)
        put("username", config.username)
        put("password", config.password)
        put("bypass", stringsJson(config.bypass))
    }
}

private fun proxyJson(proxy: Proxy): JsonObject = buildJsonObject {
    put("type", proxy.type().name)
    if (proxy.type() != Proxy.Type.DIRECT) {
        val address = proxy.address() as InetSocketAddress
        put("host", address.hostString)
        put("port", address.port)
    }
}

private fun policyJson(policy: ClientPolicy): JsonObject = buildJsonObject {
    put("connectTimeoutSeconds", policy.connectTimeoutSeconds)
    put("readTimeoutSeconds", policy.readTimeoutSeconds)
    put("writeTimeoutSeconds", policy.writeTimeoutSeconds)
    put("retryOnConnectionFailure", policy.retryOnConnectionFailure)
    put("followRedirects", policy.followRedirects)
    put("followSslRedirects", policy.followSslRedirects)
}

private fun headersJson(headers: List<Pair<String, String>>): JsonArray = buildJsonArray {
    for ((name, value) in headers) {
        add(buildJsonArray {
            add(name)
            add(value)
        })
    }
}

private fun stringsJson(values: List<String>): JsonArray = buildJsonArray {
    for (value in values) add(value)
}

private fun JsonObject.string(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: error("missing string $name")

private fun JsonObject.optionalString(name: String): String =
    (this[name] as? JsonPrimitive)?.content ?: ""

private fun JsonObject.boolean(name: String): Boolean =
    (this[name] as? JsonPrimitive)?.content?.toBooleanStrictOrNull()
        ?: error("missing boolean $name")

private fun JsonObject.strings(name: String): List<String> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { it.jsonPrimitive.content }
}

private fun JsonObject.pairs(name: String): List<Pair<String, String>> {
    val array = this[name] as? JsonArray ?: return emptyList()
    return array.map { element ->
        val pair = element.jsonArray
        pair[0].jsonPrimitive.content to pair[1].jsonPrimitive.content
    }
}

private fun JsonObject.proxyConfig(name: String): ProxyConfig? {
    val element = this[name] ?: return null
    if (element is JsonNull) return null
    val value = element.jsonObject
    return ProxyConfig(
        type = when (value.string("type")) {
            "HTTP" -> ProxyType.HTTP
            "SOCKS" -> ProxyType.SOCKS
            else -> error("unknown proxy type")
        },
        host = value.string("host"),
        port = value.string("port").toInt(),
        username = value.optionalString("username"),
        password = value.optionalString("password"),
        bypass = value.strings("bypass"),
    )
}
