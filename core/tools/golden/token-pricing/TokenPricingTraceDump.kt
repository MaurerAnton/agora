// Golden trace driver for the token-pricing cluster:
//
//   api/util/tokens/ImageTokenCost.kt          ImageDescriptor, ByteProportionalImageCost
//   api/util/tokens/PixelImageTokenCost.kt     pixel fallback, blocksCovering, proportionalEdge
//   api/util/tokens/AnthropicModelId.kt        id parsing and version comparison
//   api/util/tokens/AnthropicImageCost.kt      28x28 patch pricing with tier limits
//   api/util/tokens/GeminiImageCost.kt         384 px single tile, 1.5 crop unit
//   api/util/tokens/OpenAiImageCost.kt         patch/tile pricing and the model table
//   api/util/tokens/FamilyEnvelopeCosts.kt     EnvelopeCost, SafetyMargin, Anthropic/OpenAI framing
//   api/util/tokens/TextTokenCounter.kt        heuristic counter
//   api/util/tokens/LocalModelTextCounter.kt   exact-with-fallback counter
//   api/util/tokens/ContextCostModel.kt        the default bundle
//
// All ten files compile VERBATIM from the app tree; the only imports are kotlin.math in three of
// them, i.e. stdlib, so the stand needs no stubs and no extraction.
//
// The driver prints raw counts and Kotlin data-class `toString()` for parsed ids and envelope
// costs. The interesting pins are the boundaries: the pixel fallback when either dimension is
// missing, the Anthropic downscale bisection, gemini's crop-unit truncation, OpenAI's shrink
// correction, and the safety margin's round-up-then-clamp.
package tokens.pricing.golden

import com.newoether.agora.api.util.tokens.AnthropicEnvelopeCosts
import com.newoether.agora.api.util.tokens.AnthropicImageCost
import com.newoether.agora.api.util.tokens.AnthropicModelId
import com.newoether.agora.api.util.tokens.ByteProportionalImageCost
import com.newoether.agora.api.util.tokens.ContextCostModel
import com.newoether.agora.api.util.tokens.EnvelopeCost
import com.newoether.agora.api.util.tokens.GeminiImageCost
import com.newoether.agora.api.util.tokens.HeuristicTextTokenCounter
import com.newoether.agora.api.util.tokens.ImageDescriptor
import com.newoether.agora.api.util.tokens.LocalModelTextCounter
import com.newoether.agora.api.util.tokens.OpenAiEnvelopeCost
import com.newoether.agora.api.util.tokens.OpenAiImageCosts
import com.newoether.agora.api.util.tokens.SafetyMargin
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

private fun opt(value: Any?): String = value?.let { esc(it.toString()) } ?: "-"

private fun descriptor(value: JsonObject): ImageDescriptor = ImageDescriptor(
    byteSize = value.number("byteSize"),
    pixelWidth = value.number("pixelWidth")?.toInt(),
    pixelHeight = value.number("pixelHeight")?.toInt(),
)

fun main(args: Array<String>) {
    val root = Json.parseToJsonElement(File(args[0]).readText()) as JsonObject
    val name = root.text("name") ?: ""
    val op = root.requiredText("op")
    val image = (root["image"] as? JsonObject)?.let(::descriptor)

    val out = StringBuilder()
    out.append("name|").append(esc(name)).append('\n')
    out.append("op|").append(esc(op)).append('\n')

    when (op) {
        "heuristic" -> {
            val text = root.requiredText("text")
            out.append("args|").append(esc(text)).append('\n')
            out.append("result|").append(HeuristicTextTokenCounter.count(text)).append('\n')
        }
        "local" -> {
            val text = root.requiredText("text")
            val exact = root.number("exact")?.toInt()
            val throws = root.flag("throws") ?: false
            out.append("args|").append(esc(text)).append('|').append(opt(exact)).append('|')
                .append(throws).append('\n')
            val counter = LocalModelTextCounter(exactCount = {
                if (throws) error("exact count unavailable") else exact
            })
            out.append("result|").append(counter.count(text)).append('\n')
        }
        "byteImage" -> {
            out.append("args|").append(opt(image?.byteSize)).append('|')
                .append(opt(image?.pixelWidth)).append('|').append(opt(image?.pixelHeight))
                .append('\n')
            out.append("result|").append(ByteProportionalImageCost.tokens(image!!)).append('\n')
        }
        "anthropicTier" -> {
            val model = root.requiredText("model")
            out.append("args|").append(esc(model)).append('\n')
            out.append("result|").append(AnthropicImageCost.tierForModelName(model).name).append('\n')
        }
        "anthropicImage" -> {
            val model = root.requiredText("model")
            out.append("args|").append(esc(model)).append('|').append(esc(image.toString())).append('\n')
            out.append("result|").append(AnthropicImageCost.forModelName(model).tokens(image!!))
                .append('\n')
        }
        "geminiImage" -> {
            out.append("args|").append(esc(image.toString())).append('\n')
            out.append("result|").append(GeminiImageCost.tokens(image!!)).append('\n')
        }
        "openaiImage" -> {
            val model = root.requiredText("model")
            out.append("args|").append(esc(model)).append('|').append(esc(image.toString())).append('\n')
            out.append("result|").append(OpenAiImageCosts.forModelName(model).tokens(image!!))
                .append('\n')
        }
        "anthropicModelId" -> {
            val model = root.requiredText("model")
            out.append("args|").append(esc(model)).append('\n')
            out.append("result|").append(esc(AnthropicModelId.parse(model).toString())).append('\n')
        }
        "versionAtLeast" -> {
            val model = root.requiredText("model")
            val major = root.number("major")?.toInt() ?: 0
            val minor = root.number("minor")?.toInt() ?: 0
            out.append("args|").append(esc(model)).append('|').append(major).append('|').append(minor)
                .append('\n')
            out.append("result|")
                .append(AnthropicModelId.parse(model).versionAtLeast(major, minor))
                .append('\n')
        }
        "envelopeDefault" -> {
            out.append("args|\n")
            out.append("result|").append(esc(EnvelopeCost.Default.toString())).append('\n')
        }
        "envelopeOpenAi" -> {
            out.append("args|\n")
            out.append("result|").append(esc(OpenAiEnvelopeCost.toString())).append('\n')
        }
        "envelopeAnthropic" -> {
            val model = root.requiredText("model")
            out.append("args|").append(esc(model)).append('\n')
            out.append("result|").append(esc(AnthropicEnvelopeCosts.forModelName(model).toString()))
                .append('\n')
        }
        "margin" -> {
            val raw = root.number("raw") ?: 0L
            out.append("args|").append(raw).append('\n')
            out.append("result|").append(SafetyMargin.Default.apply(raw)).append('\n')
        }
        "defaultModel" -> {
            val text = root.requiredText("text")
            val raw = root.number("raw") ?: 0L
            out.append("args|").append(esc(text)).append('|').append(esc(image.toString())).append('|')
                .append(raw).append('\n')
            val model = ContextCostModel.Default
            out.append("result|")
                .append(model.text.count(text)).append('|')
                .append(model.image.tokens(image!!)).append('|')
                .append(esc(model.envelope.toString())).append('|')
                .append(model.safetyMargin.apply(raw))
                .append('\n')
        }
        else -> error("unknown op: $op")
    }
    print(out)
}
