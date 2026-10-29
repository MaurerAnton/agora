// Golden trace driver for data/ModelThinkingCapabilityResolver.kt.
//
// Compiles the real resolver, ModelId, the thinking-capability cluster and CustomProviderConfig
// from the app tree. CustomProviderIdentityPolicy.isStableId is generated from the real file by
// `agora_extract identity-policy` (the rest of that file is Room/model-migration code).
//
// The trace pins the parsed provider/model pair, the first matching custom provider, its
// normalized protocol, and every field of the resulting ModelThinkingCapability.
package model.thinking.resolver.golden

import com.newoether.agora.data.CustomEndpointProtocol
import com.newoether.agora.data.CustomProviderConfig
import com.newoether.agora.data.thinkingCapabilityForSelectedModel
import com.newoether.agora.model.ModelId
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

private fun opt(value: String?): String = value?.let(::esc) ?: "-"

private fun JsonObject.str(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.requiredStr(name: String): String =
    str(name) ?: error("scenario field `$name` is required")

private fun JsonObject.bool(name: String): Boolean =
    this[name]?.jsonPrimitive?.boolean ?: false

private fun JsonObject.provider(): CustomProviderConfig = CustomProviderConfig(
    name = requiredStr("name"),
    protocol = CustomEndpointProtocol.fromWireValue(requiredStr("protocol")),
    id = str("id") ?: "",
    legacyNames = (this["legacyNames"] as? JsonArray)
        ?.map { (it as JsonPrimitive).content }
        ?.toSet()
        ?: emptySet(),
)

private fun JsonObject.providers(): List<CustomProviderConfig> =
    (this["customProviders"] as? JsonArray)?.map { it.jsonObject.provider() } ?: emptyList()

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()).jsonObject
    val name = root.str("name") ?: File(args[0]).nameWithoutExtension
    val selected = root.requiredStr("selectedModel")
    val providers = root.providers()

    val parsed = ModelId.parse(selected)
    val capability = thinkingCapabilityForSelectedModel(selected, providers)

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("selected|").append(esc(selected)).append('\n')
    out.append("parsed|").append(esc(parsed.providerName)).append('|')
        .append(esc(parsed.modelName)).append('\n')
    providers.forEachIndexed { index, provider ->
        out.append("provider|").append(index).append('|')
            .append(esc(provider.name)).append('|')
            .append(provider.protocol.wireValue).append('|')
            .append(esc(provider.id)).append('|')
            .append(provider.legacyNames.joinToString(",") { esc(it) }).append('\n')
    }
    out.append("capability|canDisableThinking|").append(capability.canDisableThinking).append('\n')
    out.append("capability|supportedEfforts|")
        .append(capability.supportedEfforts.joinToString(",") { esc(it) }).append('\n')
    out.append("capability|supportsThinkingBudget|")
        .append(capability.supportsThinkingBudget).append('\n')
    out.append("capability|minBudgetTokens|").append(capability.minBudgetTokens).append('\n')
    out.append("capability|maxBudgetTokens|")
        .append(capability.maxBudgetTokens?.toString() ?: "-").append('\n')
    out.append("capability|supportsEffortWithBudget|")
        .append(capability.supportsEffortWithBudget).append('\n')
    out.append("capability|supportsSamplingParams|")
        .append(capability.supportsSamplingParams).append('\n')
    out.append("capability|supportsEffort|").append(capability.supportsEffort).append('\n')
    out.append("capability|lowestEffort|").append(opt(capability.lowestEffort)).append('\n')
    print(out)
}
