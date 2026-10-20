// Golden trace driver for the provider-protocol helpers in data/CustomProviderConfig.kt.
//
// Compiles the real CustomProviderConfig.kt (and its generated isStableId slice). Each trace prints
// every helper result for one provider name, the built-in switches and an ordered custom list.
package provider.protocol.support.golden

import com.newoether.agora.data.CustomEndpointProtocol
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.anthropicCacheTtlForProvider
import com.newoether.agora.data.isAnthropicCacheEnabledForProvider
import com.newoether.agora.data.isAnthropicProtocolProvider
import com.newoether.agora.data.isOpenAiProtocolProvider
import com.newoether.agora.data.isPromptCacheKeyEnabledForProvider
import com.newoether.agora.data.isPromptCacheSettingEnabledForProvider
import com.newoether.agora.data.isResponsesApiEnabledForProvider
import com.newoether.agora.data.supportsPromptCacheSetting
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonArray
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.boolean
import kotlinx.serialization.json.jsonObject
import kotlinx.serialization.json.jsonPrimitive
import java.io.File

private fun esc(value: String): String {
    val out = StringBuilder(value.length)
    for (c in value) {
        when (c) {
            '\\' -> out.append("\\\\")
            '|' -> out.append("\\p")
            ';' -> out.append("\\s")
            '\n' -> out.append("\\n")
            '\r' -> out.append("\\r")
            '\t' -> out.append("\\t")
            else -> out.append(c)
        }
    }
    return out.toString()
}

private fun JsonObject.str(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.requiredStr(name: String): String =
    str(name) ?: error("scenario field `$name` is required")

private fun JsonObject.bool(name: String, fallback: Boolean = true): Boolean =
    (this[name] as? JsonPrimitive)?.boolean ?: fallback

private fun JsonObject.provider(): CustomProviderConfig = CustomProviderConfig(
    name = requiredStr("name"),
    protocol = CustomEndpointProtocol.fromWireValue(requiredStr("protocol")),
    id = str("id") ?: "",
    legacyNames = (this["legacyNames"] as? JsonArray)
        ?.map { (it as JsonPrimitive).content }
        ?.toSet()
        ?: emptySet(),
    responsesApiEnabled = bool("responsesApiEnabled", false),
    anthropicCacheEnabled = bool("anthropicCacheEnabled", true),
    anthropicCacheTtl = str("anthropicCacheTtl") ?: "1h",
)

private fun JsonObject.providers(): List<CustomProviderConfig> =
    (this["customProviders"] as? JsonArray)?.map { it.jsonObject.provider() } ?: emptyList()

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()).jsonObject
    val name = root.str("name") ?: File(args[0]).nameWithoutExtension
    val provider = root.requiredStr("providerName")
    val builtIn = root.bool("builtInEnabled", true)
    val builtInAnthropic = root.bool("builtInAnthropicEnabled", true)
    val builtInTtl = root.str("builtInTtl") ?: "1h"
    val providers = root.providers()

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("provider|").append(esc(provider)).append('\n')
    out.append("builtInEnabled|").append(builtIn).append('\n')
    out.append("builtInAnthropicEnabled|").append(builtInAnthropic).append('\n')
    out.append("builtInTtl|").append(esc(builtInTtl)).append('\n')
    providers.forEachIndexed { index, config ->
        out.append("custom|").append(index).append('|')
            .append(esc(config.name)).append('|')
            .append(config.protocol.wireValue).append('|')
            .append(esc(config.id)).append('|')
            .append(config.legacyNames.joinToString(",") { esc(it) }).append('|')
            .append(config.responsesApiEnabled).append('|')
            .append(config.anthropicCacheEnabled).append('|')
            .append(esc(config.anthropicCacheTtl)).append('\n')
    }
    out.append("isOpenAiProtocol|").append(isOpenAiProtocolProvider(provider, providers)).append('\n')
    out.append("isAnthropicProtocol|").append(isAnthropicProtocolProvider(provider, providers)).append('\n')
    out.append("isAnthropicCacheEnabled|")
        .append(isAnthropicCacheEnabledForProvider(provider, builtIn, providers)).append('\n')
    out.append("supportsPromptCache|")
        .append(supportsPromptCacheSetting(provider, providers)).append('\n')
    out.append("isPromptCacheKeyEnabled|")
        .append(isPromptCacheKeyEnabledForProvider(provider, providers)).append('\n')
    out.append("isPromptCacheSettingEnabled|")
        .append(isPromptCacheSettingEnabledForProvider(provider, builtInAnthropic, providers)).append('\n')
    out.append("anthropicCacheTtl|")
        .append(esc(anthropicCacheTtlForProvider(provider, builtInTtl, providers))).append('\n')
    out.append("isResponsesApiEnabled|")
        .append(isResponsesApiEnabledForProvider(provider, builtIn, providers)).append('\n')
    print(out)
}
