#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace agora::core::token_pricing {

/**
 * Port of the host-compilable token-pricing cluster under api/util/tokens:
 * ImageTokenCost.kt, PixelImageTokenCost.kt, AnthropicModelId.kt, AnthropicImageCost.kt,
 * GeminiImageCost.kt, OpenAiImageCost.kt, FamilyEnvelopeCosts.kt, TextTokenCounter.kt,
 * LocalModelTextCounter.kt and ContextCostModel.kt.
 *
 * Boundary note: the Kotlin parsers call `String.lowercase()` (Unicode-aware) and
 * `Character.isLetterOrDigit` only for ASCII code points. This port folds ASCII A-Z only, like the
 * other core ports; model ids are ASCII in practice and the scenarios use ASCII letters. The
 * heuristic counter does use the JVM whitespace table, including its deliberate NBSP exclusion.
 */

/** Everything known offline about one provider-visible image. */
struct ImageDescriptor {
    std::optional<std::int64_t> byte_size;
    std::optional<int> pixel_width;
    std::optional<int> pixel_height;
};

inline constexpr std::int64_t kBytesPerToken = 768;
inline constexpr std::int64_t kMinImageTokens = 256;
inline constexpr std::int64_t kMaxImageTokens = 1536;
inline constexpr std::int64_t kUnknownImageTokens = 1024;

/** ByteProportionalImageCost: the model-independent fallback. */
std::int64_t byte_proportional_tokens(const ImageDescriptor& image);

/** Whole blocks of `divisor` needed to cover `value`. */
std::int64_t blocks_covering(int value, int divisor);

/** Aspect-preserving companion dimension, rounded like Math.round. */
int proportional_edge(int edge, int from_edge, int to_edge);

/** AnthropicModelId.parse: tier, version pair, version comparison. */
struct AnthropicModelId {
    enum class Tier { Opus, Sonnet, Haiku };

    std::optional<Tier> tier;
    std::optional<int> major;
    int minor = 0;

    bool version_at_least(int other_major, int other_minor) const;
    static AnthropicModelId parse(std::string_view model_name);
};

/** Anthropic image tier limits: STANDARD before Claude 4.7, HIGH_RESOLUTION from 4.7. */
enum class AnthropicImageTier { Standard, HighResolution };

AnthropicImageTier anthropic_tier_for_model_name(std::string_view model_name);

/** 28x28 visual tokens with the tier's long-edge and visual-token caps. */
std::int64_t anthropic_image_tokens(AnthropicImageTier tier, const ImageDescriptor& image);

/** Fixed 258-token crops; 384 px square is the single-tile threshold. */
std::int64_t gemini_image_tokens(const ImageDescriptor& image);

/** Patch/tile rules selected by the published OpenAI model table. */
std::int64_t openai_image_tokens(std::string_view model_name, const ImageDescriptor& image);

/** Provider framing cost. */
struct EnvelopeCost {
    int per_message;
    int per_tool_definition;
    int per_tool_call;
    int request_overhead = 0;
    int tool_set_overhead = 0;
};

EnvelopeCost default_envelope();
EnvelopeCost openai_envelope();

/** Anthropic's tool-use system prompt overhead for the model, or the largest published value. */
EnvelopeCost anthropic_envelope(std::string_view model_name);

/** Final upward correction applied to a raw count. */
struct SafetyMargin {
    std::int64_t numerator;
    std::int64_t denominator;

    int apply(std::int64_t raw) const;
};

SafetyMargin default_safety_margin();

/** HeuristicTextTokenCounter: ASCII runs cost ceil(chars / 4), other code points cost one. */
std::int64_t heuristic_text_tokens(std::string_view text);

/** LocalModelTextCounter: exact count when available and non-negative, heuristic otherwise. */
std::int64_t local_model_text_tokens(std::string_view text, std::optional<int> exact_count,
                                     bool exact_throws);

/** The complete price list an estimator needs (ContextCostModel.Default wiring). */
struct ContextCostModel {
    std::int64_t (*text)(std::string_view) = nullptr;
    std::int64_t (*image)(const ImageDescriptor&) = nullptr;
    EnvelopeCost envelope;
    SafetyMargin safety_margin{};
};

ContextCostModel default_context_cost_model();

/** Kotlin data-class `toString()` reproductions for the golden traces. */
std::string describe(const ImageDescriptor& image);
std::string describe(const AnthropicModelId& model);
std::string describe(AnthropicImageTier tier);
std::string describe(const EnvelopeCost& envelope);

}  // namespace agora::core::token_pricing
