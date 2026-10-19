// Golden trace driver for the thinking-capability cluster:
//
//   model/ModelThinkingCapability.kt          ModelThinkingCapability, ResolvedThinking, ThinkingResolution
//   model/ThinkingControl.kt                  ThinkingLevels
//   model/ModelThinkingCapabilityDefaults.kt  ThinkingProviderFamily, built-in defaults
//   model/ModelThinkingCapabilities.kt        familyFor / forModel / forProvider
//
// All four files compile VERBATIM from the app tree (no imports at all), so this stand needs no
// stubs and no extraction. Each scenario exercises one operation and prints its result.
//
// Capability and resolution results are printed with Kotlin's data-class `toString()`: the deltas
// between the built-in entries (effort subsets, budget ceilings, `supportsEffortWithBudget`,
// sampling support) are exactly what the wire payload depends on, so pinning every field in
// declaration order is the point.
package thinking.capability.golden

import com.newoether.agora.model.ModelThinkingCapability
import com.newoether.agora.model.ModelThinkingCapabilities
import com.newoether.agora.model.ModelThinkingCapabilityDefaults
import com.newoether.agora.model.ResolvedThinking
import com.newoether.agora.model.ThinkingLevels
import com.newoether.agora.model.ThinkingProviderFamily
import com.newoether.agora.model.ThinkingResolution
import kotlinx.serialization.json.Json
import kotlinx.serialization.json.JsonObject
import kotlinx.serialization.json.JsonPrimitive
import kotlinx.serialization.json.booleanOrNull
import kotlinx.serialization.json.longOrNull
import java.io.File

/** Field-safe escaping: no raw separators or control characters survive. */
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

private fun JsonObject.text(name: String): String? =
    (this[name] as? JsonPrimitive)?.takeIf { it.isString }?.content

private fun JsonObject.number(name: String): Long? =
    (this[name] as? JsonPrimitive)?.longOrNull

private fun JsonObject.flag(name: String): Boolean? =
    (this[name] as? JsonPrimitive)?.booleanOrNull

private fun JsonObject.requiredText(name: String): String =
    text(name) ?: error("scenario field `$name` is required")

private fun optOrDash(value: String?): String = value?.let(::esc) ?: "-"

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    fun family(value: String): ThinkingProviderFamily = ThinkingProviderFamily.valueOf(value)

    when (op) {
        "normalize" -> {
            val raw = root.text("raw")
            out.append("args|").append(optOrDash(raw)).append('\n')
            out.append("result|").append(esc(ThinkingLevels.normalize(raw))).append('\n')
        }
        "legacyBudget" -> {
            val raw = root.text("raw")
            out.append("args|").append(optOrDash(raw)).append('\n')
            out.append("result|").append(ThinkingLevels.legacyBudgetTokens(raw)?.toString() ?: "null")
                .append('\n')
        }
        "effortForIndex" -> {
            val index = root.number("index")?.toInt() ?: 0
            out.append("args|").append(index).append('\n')
            out.append("result|").append(esc(ThinkingLevels.effortForIndex(index))).append('\n')
        }
        "indexForEffort" -> {
            val effort = root.requiredText("effort")
            out.append("args|").append(esc(effort)).append('\n')
            out.append("result|").append(ThinkingLevels.indexForEffort(effort)).append('\n')
        }
        "family" -> {
            val provider = root.text("providerName")
            val protocol = root.text("customProtocol")
            out.append("args|").append(optOrDash(provider)).append('|').append(optOrDash(protocol))
                .append('\n')
            out.append("result|").append(ModelThinkingCapabilities.familyFor(provider, protocol).name)
                .append('\n')
        }
        "familyDefault" -> {
            val familyName = root.requiredText("family")
            out.append("args|").append(esc(familyName)).append('\n')
            out.append("result|")
                .append(esc(ModelThinkingCapabilityDefaults.familyDefault(family(familyName)).toString()))
                .append('\n')
        }
        "capability" -> {
            val familyName = root.requiredText("family")
            val modelId = root.requiredText("modelId")
            out.append("args|").append(esc(familyName)).append('|').append(esc(modelId)).append('\n')
            out.append("result|")
                .append(esc(ModelThinkingCapabilityDefaults.forModel(family(familyName), modelId).toString()))
                .append('\n')
        }
        "providerCapability" -> {
            val provider = root.text("providerName")
            val protocol = root.text("customProtocol")
            val modelId = root.requiredText("modelId")
            out.append("args|").append(optOrDash(provider)).append('|').append(optOrDash(protocol))
                .append('|').append(esc(modelId)).append('\n')
            out.append("result|")
                .append(esc(ModelThinkingCapabilities.forProvider(provider, modelId, protocol).toString()))
                .append('\n')
        }
        "nearest" -> {
            val familyName = root.requiredText("family")
            val modelId = root.requiredText("modelId")
            val effort = root.requiredText("effort")
            out.append("args|").append(esc(familyName)).append('|').append(esc(modelId))
                .append('|').append(esc(effort)).append('\n')
            val capability = ModelThinkingCapabilityDefaults.forModel(family(familyName), modelId)
            out.append("result|").append(optOrDash(capability.nearestEffort(effort))).append('\n')
        }
        "clamp" -> {
            val familyName = root.requiredText("family")
            val modelId = root.requiredText("modelId")
            val tokens = root.number("tokens")?.toInt() ?: 0
            out.append("args|").append(esc(familyName)).append('|').append(esc(modelId))
                .append('|').append(tokens).append('\n')
            val capability = ModelThinkingCapabilityDefaults.forModel(family(familyName), modelId)
            out.append("result|").append(capability.clampBudget(tokens)).append('\n')
        }
        "resolve" -> {
            val familyName = root.requiredText("family")
            val modelId = root.requiredText("modelId")
            val enabled = root.flag("enabled") ?: false
            val effort = root.requiredText("effort")
            val budgetEnabled = root.flag("budgetEnabled") ?: false
            val budgetTokens = root.number("budgetTokens")?.toInt() ?: 0
            out.append("args|").append(esc(familyName)).append('|').append(esc(modelId))
                .append('|').append(enabled).append('|').append(esc(effort))
                .append('|').append(budgetEnabled).append('|').append(budgetTokens).append('\n')
            val capability = ModelThinkingCapabilityDefaults.forModel(family(familyName), modelId)
            val resolved: ResolvedThinking = ThinkingResolution.resolve(
                capability = capability,
                requestedEnabled = enabled,
                requestedEffort = effort,
                requestedBudgetEnabled = budgetEnabled,
                requestedBudgetTokens = budgetTokens,
            )
            out.append("result|").append(esc(resolved.toString())).append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
