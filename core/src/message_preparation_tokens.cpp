// The token-cost half of the message-preparation port: counters, image rules, per-family
// envelopes and the CostModelContextEstimator walk. Mirrors api/util/tokens/* exactly — the
// golden traces under core/tools/golden/message-preparation pin every number.
#include <algorithm>
#include <cmath>
#include <cctype>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/message_preparation.hpp"
#include "agora/core/unicode.hpp"

namespace agora::core::prep {
namespace {

// -- shared geometry helpers (PixelImageTokenCost.kt) -----------------------------------------------

/** Whole blocks of divisor needed to cover value. */
std::int64_t blocks_covering(std::int32_t value, std::int32_t divisor) {
    if (value <= 0) return 0;
    return (static_cast<std::int64_t>(value) + divisor - 1) / divisor;
}

/** Aspect-preserving companion dimension (Java Math.round == floor(x + 0.5) for positives). */
std::int32_t proportional_edge(std::int32_t edge, std::int32_t from_edge, std::int32_t to_edge) {
    if (from_edge <= 0) return 1;
    const auto rounded = static_cast<std::int32_t>(
        std::floor(static_cast<double>(edge) * to_edge / from_edge + 0.5));
    return std::max(rounded, 1);
}

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool contains_ascii(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

// -- AnthropicModelId.kt ---------------------------------------------------------------------------
// VERSION_PAIR = (?<!\d)(\d{1,2})[-.](\d{1,2})(?!\d); MAJOR_ONLY = (?<![\d.])(\d{1,2})(?![\d.])

struct AnthropicModelId {
    enum class Tier { Opus, Sonnet, Haiku };
    std::optional<Tier> tier;
    std::optional<std::int32_t> major;
    std::int32_t minor = 0;
};

AnthropicModelId parse_anthropic_model_id(const std::string& model_name) {
    const std::size_t slash = model_name.rfind('/');
    const std::string name = lower_ascii(
        slash == std::string::npos ? model_name : model_name.substr(slash + 1));

    AnthropicModelId result;
    if (contains_ascii(name, "opus")) {
        result.tier = AnthropicModelId::Tier::Opus;
    } else if (contains_ascii(name, "sonnet")) {
        result.tier = AnthropicModelId::Tier::Sonnet;
    } else if (contains_ascii(name, "haiku")) {
        result.tier = AnthropicModelId::Tier::Haiku;
    }

    // First VERSION_PAIR match: up to two digits, '-', up to two digits, no adjacent digits.
    for (std::size_t start = 0; start < name.size(); ++start) {
        if (!is_ascii_digit(name[start])) continue;
        if (start > 0 && is_ascii_digit(name[start - 1])) continue;
        std::size_t cursor = start;
        std::string major_digits;
        major_digits.push_back(name[cursor]);
        ++cursor;
        if (cursor < name.size() && is_ascii_digit(name[cursor])) {
            major_digits.push_back(name[cursor]);
            ++cursor;
        }
        if (cursor >= name.size() || (name[cursor] != '-' && name[cursor] != '.')) continue;
        ++cursor;
        if (cursor >= name.size() || !is_ascii_digit(name[cursor])) continue;
        std::string minor_digits;
        minor_digits.push_back(name[cursor]);
        ++cursor;
        if (cursor < name.size() && is_ascii_digit(name[cursor])) {
            minor_digits.push_back(name[cursor]);
            ++cursor;
        }
        if (cursor < name.size() && is_ascii_digit(name[cursor])) continue;  // (?!\d)
        result.major = std::stoi(major_digits);
        result.minor = std::stoi(minor_digits);
        return result;
    }

    // MAJOR_ONLY: up to two digits not adjacent to digits or dots.
    for (std::size_t start = 0; start < name.size(); ++start) {
        if (!is_ascii_digit(name[start])) continue;
        if (start > 0 && (is_ascii_digit(name[start - 1]) || name[start - 1] == '.')) continue;
        std::size_t cursor = start;
        std::string digits;
        digits.push_back(name[cursor]);
        ++cursor;
        if (cursor < name.size() && is_ascii_digit(name[cursor])) {
            digits.push_back(name[cursor]);
            ++cursor;
        }
        if (cursor < name.size() && (is_ascii_digit(name[cursor]) || name[cursor] == '.')) {
            continue;  // (?![\d.])
        }
        result.major = std::stoi(digits);
        return result;
    }
    return result;
}

bool anthropic_version_at_least(const AnthropicModelId& id, std::int32_t major, std::int32_t minor) {
    if (!id.major) return false;
    return *id.major > major || (*id.major == major && id.minor >= minor);
}

/** AnthropicEnvelopeCosts: the published tool-use system prompt tokens by tier and version. */
std::int64_t anthropic_tool_use_system_prompt(const AnthropicModelId& id) {
    struct Entry {
        AnthropicModelId::Tier tier;
        std::int32_t version;  // scaled by ten: 4.5 is 45
        std::int64_t tokens;
    };
    static const Entry kTable[] = {
        {AnthropicModelId::Tier::Opus, 55, 286},  {AnthropicModelId::Tier::Opus, 50, 286},
        {AnthropicModelId::Tier::Opus, 48, 290},  {AnthropicModelId::Tier::Opus, 47, 675},
        {AnthropicModelId::Tier::Opus, 46, 497},  {AnthropicModelId::Tier::Opus, 45, 496},
        {AnthropicModelId::Tier::Opus, 41, 313},  {AnthropicModelId::Tier::Opus, 40, 313},
        {AnthropicModelId::Tier::Sonnet, 50, 354}, {AnthropicModelId::Tier::Sonnet, 46, 497},
        {AnthropicModelId::Tier::Sonnet, 45, 496}, {AnthropicModelId::Tier::Sonnet, 40, 313},
        {AnthropicModelId::Tier::Haiku, 45, 496}, {AnthropicModelId::Tier::Haiku, 35, 264},
    };
    static const std::int64_t kLargestPublished = 675;  // max of the table, over-estimating is safe

    if (!id.tier || !id.major) return kLargestPublished;
    const std::int32_t version = *id.major * 10 + id.minor;
    for (const Entry& entry : kTable) {
        if (entry.tier == *id.tier && entry.version == version) return entry.tokens;
    }
    return kLargestPublished;
}

/** OpenAiImageCosts.forModelName: the published family table, conservative default. */
std::int64_t openai_image_tokens_for_model(const ImageDescriptor& image,
                                           const std::string& model_name) {
    const std::size_t slash = model_name.rfind('/');
    const std::string name = lower_ascii(
        slash == std::string::npos ? model_name : model_name.substr(slash + 1));
    if (!image.pixel_width || !image.pixel_height || *image.pixel_width <= 0 || *image.pixel_height <= 0) {
        return byte_proportional_image_tokens(image);
    }
    const std::int32_t width = *image.pixel_width;
    const std::int32_t height = *image.pixel_height;
    if (starts_with(name, "gpt-4o-mini")) return openai_tile_image_tokens(width, height, 2833, 5667);
    if (starts_with(name, "gpt-4o")) return openai_tile_image_tokens(width, height, 85, 170);
    if (starts_with(name, "gpt-4.1-mini")) return openai_patch_image_tokens(width, height, 2048, 6144, 1.62);
    if (starts_with(name, "gpt-4.1-nano")) return openai_patch_image_tokens(width, height, 2048, 6144, 2.46);
    if (starts_with(name, "gpt-4.1")) return openai_tile_image_tokens(width, height, 85, 170);
    if (starts_with(name, "gpt-5.1")) return openai_tile_image_tokens(width, height, 70, 140);
    if (starts_with(name, "gpt-5.2")) return openai_patch_image_tokens(width, height, 2048, 6144, 1.2);
    if (starts_with(name, "gpt-5.4")) return openai_patch_image_tokens(width, height, 2048, 2500, 1.2);
    if (starts_with(name, "gpt-5.5")) return openai_patch_image_tokens(width, height, 6000, 10000, 1.2);
    if (starts_with(name, "gpt-5-nano")) return openai_patch_image_tokens(width, height, 2048, 2500, 1.5);
    if (starts_with(name, "gpt-5-mini")) return openai_patch_image_tokens(width, height, 2048, 2500, 1.2);
    if (starts_with(name, "gpt-5")) return openai_tile_image_tokens(width, height, 70, 140);
    if (starts_with(name, "o4-mini")) return openai_patch_image_tokens(width, height, 2048, 2500, 1.72);
    if (starts_with(name, "o1") || starts_with(name, "o3")) {
        return openai_tile_image_tokens(width, height, 75, 150);
    }
    // PATCH_DEFAULT: no shrink budget, the shared 1.2 multiplier.
    return openai_patch_image_tokens(width, height, 65535, std::nullopt, 1.2);
}

}  // namespace

// -- EnvelopeCost / SafetyMargin / ContextCostModel -------------------------------------------------

EnvelopeCost EnvelopeCost::default_cost() {
    return EnvelopeCost{8, 16, 16, 0, 0};
}

int SafetyMargin::apply(std::int64_t raw) const {
    // Rounds up, then clamps like Kotlin's coerceIn(0, Int.MAX_VALUE).toInt().
    const std::int64_t scaled = raw * numerator + denominator - 1;
    const std::int64_t rounded = scaled / denominator;
    const std::int64_t clamped = std::clamp(
        rounded, std::int64_t{0},
        static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
    return static_cast<int>(clamped);
}

ContextCostModel ContextCostModel::default_model() {
    ContextCostModel model;
    model.text = [](std::string_view text) { return heuristic_count(text); };
    model.image = [](const ImageDescriptor& image) { return byte_proportional_image_tokens(image); };
    model.envelope = EnvelopeCost::default_cost();
    model.safety_margin = SafetyMargin{};
    return model;
}

std::int64_t heuristic_count(std::string_view text) {
    if (text.empty()) return 0;
    std::int64_t tokens = 0;
    std::int32_t ascii_run = 0;
    const auto flush = [&] {
        if (ascii_run > 0) {
            tokens += (ascii_run + 3) / 4;  // ceil(asciiRun / 4.0)
            ascii_run = 0;
        }
    };
    std::size_t index = 0;
    while (index < text.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(text.data() + index, text.size() - index);
        const std::uint32_t code_point = decoded.code_point;
        index += decoded.length;
        if (code_point <= 0x7F &&
            (unicode::is_letter(code_point) || unicode::is_number(code_point))) {
            ++ascii_run;
        } else if (unicode::is_jvm_whitespace(code_point)) {
            flush();
        } else {
            flush();
            ++tokens;
        }
    }
    flush();
    return tokens;
}

std::int64_t byte_proportional_image_tokens(const ImageDescriptor& image) {
    static constexpr std::int64_t kBytesPerToken = 768;
    static constexpr std::int64_t kMinTokens = 256;
    static constexpr std::int64_t kMaxTokens = 1536;
    static constexpr std::int64_t kUnknownTokens = 1024;
    if (!image.byte_size || *image.byte_size <= 0) return kUnknownTokens;
    return std::clamp(*image.byte_size / kBytesPerToken, kMinTokens, kMaxTokens);
}

std::int64_t openai_tile_image_tokens(std::int32_t width, std::int32_t height,
                                      std::int32_t base_tokens, std::int32_t tile_tokens) {
    static constexpr std::int32_t kMaxSquarePx = 2048;
    static constexpr std::int32_t kShortEdgePx = 768;
    static constexpr std::int32_t kTilePx = 512;
    std::int32_t fitted_width = width;
    std::int32_t fitted_height = height;
    const std::int32_t long_edge = std::max(width, height);
    if (long_edge > kMaxSquarePx) {
        if (width >= height) {
            fitted_width = kMaxSquarePx;
            fitted_height = proportional_edge(height, width, kMaxSquarePx);
        } else {
            fitted_height = kMaxSquarePx;
            fitted_width = proportional_edge(width, height, kMaxSquarePx);
        }
    }
    const std::int32_t short_edge = std::min(fitted_width, fitted_height);
    if (short_edge > kShortEdgePx) {
        const double ratio = static_cast<double>(kShortEdgePx) / short_edge;
        if (fitted_width <= fitted_height) {
            fitted_height = std::max(static_cast<std::int32_t>(std::floor(fitted_height * ratio)), 1);
            fitted_width = kShortEdgePx;
        } else {
            fitted_width = std::max(static_cast<std::int32_t>(std::floor(fitted_width * ratio)), 1);
            fitted_height = kShortEdgePx;
        }
    }
    const std::int64_t tiles = blocks_covering(fitted_width, kTilePx) * blocks_covering(fitted_height, kTilePx);
    return base_tokens + tiles * tile_tokens;
}

std::int64_t openai_patch_image_tokens(std::int32_t width, std::int32_t height,
                                       std::int32_t max_edge_px,
                                       std::optional<std::int32_t> patch_budget, double multiplier) {
    static constexpr std::int32_t kPatchPx = 32;
    std::int32_t fitted_width = width;
    std::int32_t fitted_height = height;
    const std::int32_t long_edge = std::max(width, height);
    if (long_edge > max_edge_px) {
        if (width >= height) {
            fitted_width = max_edge_px;
            fitted_height = proportional_edge(height, width, max_edge_px);
        } else {
            fitted_height = max_edge_px;
            fitted_width = proportional_edge(width, height, max_edge_px);
        }
    }
    auto patches = blocks_covering(fitted_width, kPatchPx) * blocks_covering(fitted_height, kPatchPx);
    if (patch_budget && patches > *patch_budget) {
        const double area = static_cast<double>(fitted_width) * fitted_height;
        const double by_area = std::sqrt(static_cast<double>(kPatchPx) * kPatchPx * *patch_budget / area);
        const double width_patches = fitted_width * by_area / kPatchPx;
        const double height_patches = fitted_height * by_area / kPatchPx;
        const double correction =
            std::min(std::floor(width_patches) / width_patches, std::floor(height_patches) / height_patches);
        const double shrink = by_area * correction;
        const std::int32_t scaled_width =
            std::max(static_cast<std::int32_t>(std::floor(fitted_width * shrink)), 1);
        const std::int32_t scaled_height =
            std::max(static_cast<std::int32_t>(std::floor(fitted_height * shrink)), 1);
        patches = blocks_covering(scaled_width, kPatchPx) * blocks_covering(scaled_height, kPatchPx);
    }
    return static_cast<std::int64_t>(std::ceil(patches * multiplier));
}

std::int64_t anthropic_image_tokens(const ImageDescriptor& image, bool high_resolution) {
    const std::int32_t max_long_edge = high_resolution ? 2576 : 1568;
    const std::int64_t max_visual_tokens = high_resolution ? 4784 : 1568;
    static constexpr std::int32_t kPatchPx = 28;
    if (!image.pixel_width || !image.pixel_height || *image.pixel_width <= 0 || *image.pixel_height <= 0) {
        return byte_proportional_image_tokens(image);
    }
    const std::int32_t width = *image.pixel_width;
    const std::int32_t height = *image.pixel_height;
    const std::int32_t long_edge = std::max(width, height);
    const std::int32_t short_edge = std::min(width, height);
    const auto visual_tokens = [&](std::int32_t scaled_long_edge) -> std::int64_t {
        const std::int32_t scaled_short_edge = proportional_edge(short_edge, long_edge, scaled_long_edge);
        return blocks_covering(scaled_long_edge, kPatchPx) * blocks_covering(scaled_short_edge, kPatchPx);
    };
    // "Largest size that fits both limits": visual tokens never fall as the long edge grows, so
    // the admissible edge is found by bisection, exactly like the reference implementation.
    std::int32_t low = 1;
    std::int32_t high = std::min(long_edge, max_long_edge);
    std::int32_t best = 1;
    while (low <= high) {
        const std::int32_t mid = low + (high - low) / 2;
        if (visual_tokens(mid) <= max_visual_tokens) {
            best = mid;
            low = mid + 1;
        } else {
            high = mid - 1;
        }
    }
    return visual_tokens(best);
}

std::int64_t gemini_image_tokens(const ImageDescriptor& image) {
    static constexpr std::int64_t kTokensPerTile = 258;
    static constexpr std::int32_t kSingleTileMaxEdgePx = 384;
    if (!image.pixel_width || !image.pixel_height || *image.pixel_width <= 0 || *image.pixel_height <= 0) {
        return byte_proportional_image_tokens(image);
    }
    const std::int32_t width = *image.pixel_width;
    const std::int32_t height = *image.pixel_height;
    if (width <= kSingleTileMaxEdgePx && height <= kSingleTileMaxEdgePx) return kTokensPerTile;
    const std::int32_t crop_unit = std::max(
        static_cast<std::int32_t>(std::floor(std::min(width, height) / 1.5)), 1);
    const std::int64_t tiles = blocks_covering(width, crop_unit) * blocks_covering(height, crop_unit);
    return std::max(tiles * kTokensPerTile, kTokensPerTile);
}

// -- family selection (ContextCostModels.kt) ---------------------------------------------------------

PrefixedModelId parse_prefixed_model_id(const std::string& prefixed) {
    const std::size_t colon = prefixed.find(':');
    if (colon != std::string::npos) {
        return PrefixedModelId{prefixed.substr(0, colon), prefixed.substr(colon + 1)};
    }
    const std::string name = lower_ascii(prefixed);
    std::string provider = "Unknown";
    if (starts_with(name, "gpt-") || starts_with(name, "o1") || starts_with(name, "o3") ||
        starts_with(name, "o4")) {
        provider = "OpenAI";
    } else if (starts_with(name, "claude-")) {
        provider = "Anthropic";
    } else if (contains_ascii(name, "deepseek")) {
        provider = "DeepSeek";
    } else if (contains_ascii(name, "qwen")) {
        provider = "Qwen";
    } else if (contains_ascii(name, "groq")) {
        provider = "Groq";
    } else if (contains_ascii(name, "models/") || starts_with(name, "gemini")) {
        provider = "Google";
    }
    return PrefixedModelId{provider, prefixed};
}

namespace {

std::optional<ContextCostFamily> family_of_provider(const std::string& provider_name) {
    const std::string name = lower_ascii(provider_name);
    if (name == "anthropic") return ContextCostFamily::Anthropic;
    if (name == "openai") return ContextCostFamily::OpenAi;
    if (name == "google") return ContextCostFamily::Gemini;
    if (name == "local") return ContextCostFamily::Local;
    return std::nullopt;
}

ContextCostFamily family_of_model_name(const std::string& model_name) {
    const std::string name = lower_ascii(model_name);
    if (contains_ascii(name, "claude")) return ContextCostFamily::Anthropic;
    if (contains_ascii(name, "gemini")) return ContextCostFamily::Gemini;
    if (contains_ascii(name, "gpt-") || contains_ascii(name, "chatgpt") ||
        starts_with(name, "o1") || starts_with(name, "o3") || starts_with(name, "o4")) {
        return ContextCostFamily::OpenAi;
    }
    return ContextCostFamily::Generic;
}

}  // namespace

ContextCostFamily family_of(const std::optional<PrefixedModelId>& model_id) {
    if (!model_id) return ContextCostFamily::Generic;
    if (const auto by_provider = family_of_provider(model_id->provider_name)) return *by_provider;
    return family_of_model_name(model_id->model_name);
}

ContextCostModel cost_model_for(const std::optional<PrefixedModelId>& model_id,
                                const TextTokenCounter& o200k_counter) {
    if (!model_id) return ContextCostModel::default_model();
    const ContextCostFamily family = family_of(model_id);
    ContextCostModel model = ContextCostModel::default_model();
    switch (family) {
        case ContextCostFamily::Local:
            // The embedded runtime's exact counter is out of this slice; before a model is
            // resident the app falls back to the heuristic, which is what the stand pins.
            model.text = [](std::string_view text) { return heuristic_count(text); };
            break;
        case ContextCostFamily::Anthropic:
        case ContextCostFamily::OpenAi:
        case ContextCostFamily::Gemini:
        case ContextCostFamily::Generic:
            model.text = o200k_counter;
            break;
    }
    switch (family) {
        case ContextCostFamily::Anthropic: {
            const std::string model_name = model_id->model_name;
            model.image = [model_name](const ImageDescriptor& image) {
                const bool high_resolution = anthropic_version_at_least(
                    parse_anthropic_model_id(model_name), 4, 7);
                return anthropic_image_tokens(image, high_resolution);
            };
            break;
        }
        case ContextCostFamily::OpenAi: {
            const std::string model_name = model_id->model_name;
            model.image = [model_name](const ImageDescriptor& image) {
                return openai_image_tokens_for_model(image, model_name);
            };
            break;
        }
        case ContextCostFamily::Gemini:
            model.image = [](const ImageDescriptor& image) { return gemini_image_tokens(image); };
            break;
        case ContextCostFamily::Local:
        case ContextCostFamily::Generic:
            model.image = [](const ImageDescriptor& image) {
                return byte_proportional_image_tokens(image);
            };
            break;
    }
    if (family == ContextCostFamily::Anthropic) {
        const std::int64_t overhead = anthropic_tool_use_system_prompt(
            parse_anthropic_model_id(model_id->model_name));
        model.envelope.tool_set_overhead = overhead;
    } else if (family == ContextCostFamily::OpenAi) {
        model.envelope = EnvelopeCost{3, 16, 16, 3, 0};
    }
    return model;
}

// -- CostModelContextEstimator ------------------------------------------------------------------------

namespace {

std::int64_t coerce_at_most_int_max(std::int64_t value) {
    return std::min(value, static_cast<std::int64_t>(std::numeric_limits<std::int32_t>::max()));
}

std::int64_t tool_property_raw(const ContextCostModel& costs, const openai::ToolProperty& property);

std::int64_t text_raw(const ContextCostModel& costs, const std::string& text) {
    return costs.text(text);
}

}  // namespace

CostModelContextEstimator::CostModelContextEstimator(ContextCostModel costs)
    : costs_(std::move(costs)) {}

int CostModelContextEstimator::estimate_text(std::string_view text) const {
    return costs_.safety_margin.apply(costs_.text(text));
}

std::int64_t CostModelContextEstimator::image_tokens(const ImageDescriptor& image) const {
    return costs_.image(image);
}

int CostModelContextEstimator::estimate(const std::vector<ChatMessage>& messages,
                                        bool include_assistant_reasoning) const {
    std::int64_t total = 0;
    for (const ChatMessage& message : messages) {
        const bool is_tool_protocol = message.is_tool_protocol_message();
        std::int64_t cost = costs_.envelope.per_message;
        if (!is_tool_protocol) cost += text_raw(costs_, message.text);
        if (!is_tool_protocol && message.participant == Participant::User) {
            // Only user-role images reach a provider; tool-result images stay display-only.
            for (std::size_t index = 0; index < message.images.size(); ++index) {
                ImageDescriptor descriptor;
                if (message.attachment_meta) {
                    // imageDescriptors: one item covers a contiguous image range.
                    for (const AttachmentItem& item : message.attachment_meta->items) {
                        if (item.type != "image" && item.type != "pdf" && item.type != "video") continue;
                        if (!item.image_index) continue;
                        const std::int32_t start = *item.image_index;
                        if (start < 0 || static_cast<std::size_t>(start) >= message.images.size()) continue;
                        const std::int32_t pages = std::max(item.page_count.value_or(1), 1);
                        const std::optional<std::int64_t> bytes_per_page =
                            item.file_size && *item.file_size > 0
                                ? std::optional<std::int64_t>(*item.file_size / pages)
                                : std::nullopt;
                        const std::optional<std::int32_t> width =
                            item.pixel_width && *item.pixel_width > 0
                                ? std::optional<std::int32_t>(*item.pixel_width)
                                : std::nullopt;
                        const std::optional<std::int32_t> height =
                            item.pixel_height && *item.pixel_height > 0
                                ? std::optional<std::int32_t>(*item.pixel_height)
                                : std::nullopt;
                        if (!bytes_per_page && (!width || !height)) continue;
                        if (static_cast<std::size_t>(start) <= index &&
                            index < static_cast<std::size_t>(start) +
                                        std::min<std::int32_t>(pages,
                                                               static_cast<std::int32_t>(message.images.size()) - start)) {
                            descriptor.byte_size = bytes_per_page;
                            descriptor.pixel_width = width;
                            descriptor.pixel_height = height;
                        }
                    }
                }
                cost += costs_.image(descriptor);
            }
        }
        if (!is_tool_protocol && include_assistant_reasoning &&
            message.participant != Participant::User && message.segments) {
            for (const MessageSegment& segment : *message.segments) {
                if (segment.type == "thought") cost += text_raw(costs_, segment.content);
            }
        }
        if (is_tool_protocol) {
            // toolProtocolRaw: thought rows and the tool payload, segments-first then legacy.
            if (message.segments) {
                for (const MessageSegment& segment : *message.segments) {
                    if (segment.type != "thought") continue;
                    cost += text_raw(costs_, segment.content);
                    cost += text_raw(costs_, segment.signature.value_or(""));
                }
            }
            std::vector<const MessageSegment*> tool_segments;
            if (message.segments) {
                for (const MessageSegment& segment : *message.segments) {
                    if (segment.type == "tool") tool_segments.push_back(&segment);
                }
            }
            if (!tool_segments.empty()) {
                for (const MessageSegment* segment : tool_segments) {
                    cost += costs_.envelope.per_tool_call;
                    cost += text_raw(costs_, segment->tool_name.value_or(""));
                    cost += text_raw(costs_, segment->tool_args.value_or(""));
                    cost += text_raw(costs_, segment->tool_result.value_or(""));
                    cost += text_raw(costs_, segment->signature.value_or(""));
                }
                for (const MessageSegment* segment : tool_segments) {
                    if (!segment->response_output_items.empty()) {
                        for (const json::Value& item : segment->response_output_items) {
                            cost += text_raw(costs_, json::to_kotlin_string(item));
                        }
                        break;
                    }
                }
            } else if (message.tool_call) {
                const ToolCallData& call = *message.tool_call;
                cost += costs_.envelope.per_tool_call;
                cost += text_raw(costs_, call.tool_name);
                cost += text_raw(costs_, call.arguments);
                cost += text_raw(costs_, call.result);
                cost += text_raw(costs_, call.signature.value_or(""));
                for (const json::Value& item : call.response_output_items) {
                    cost += text_raw(costs_, json::to_kotlin_string(item));
                }
            }
        }
        total = coerce_at_most_int_max(total + cost);
    }
    return costs_.safety_margin.apply(total);
}

int CostModelContextEstimator::estimate_fixed(
    const std::optional<std::string>& system_prompt,
    const std::vector<openai::ToolDefinition>& tools,
    const std::optional<std::string>& initial_user_prompt, bool code_execution_enabled,
    bool google_search_enabled, bool open_ai_web_search_enabled) const {
    std::int64_t prompt = costs_.envelope.request_overhead + costs_.envelope.per_message +
                          text_raw(costs_, system_prompt.value_or(""));
    if (initial_user_prompt && !json::is_blank(*initial_user_prompt)) {
        prompt += costs_.envelope.per_message + text_raw(costs_, *initial_user_prompt);
    }
    std::int64_t tool_cost = 0;
    for (const openai::ToolDefinition& tool : tools) {
        tool_cost += costs_.envelope.per_tool_definition;
        tool_cost += text_raw(costs_, tool.type);
        tool_cost += text_raw(costs_, tool.function.name);
        tool_cost += text_raw(costs_, tool.function.description);
        if (tool.function.parameters.schema) {
            tool_cost += text_raw(costs_, json::to_kotlin_string(*tool.function.parameters.schema));
        } else {
            tool_cost += text_raw(costs_, tool.function.parameters.type);
            // toSortedMap(): property names in natural order, regardless of declaration order.
            std::vector<std::pair<std::string, openai::ToolProperty>> sorted_properties =
                tool.function.parameters.properties;
            std::stable_sort(sorted_properties.begin(), sorted_properties.end(),
                             [](const auto& left, const auto& right) {
                                 return left.first < right.first;
                             });
            for (const auto& [name, property] : sorted_properties) {
                tool_cost += text_raw(costs_, name);
                tool_cost += tool_property_raw(costs_, property);
            }
            std::vector<std::string> sorted_required = tool.function.parameters.required;
            std::sort(sorted_required.begin(), sorted_required.end());
            for (const std::string& required : sorted_required) {
                tool_cost += text_raw(costs_, required);
            }
        }
    }
    std::vector<std::string> native_tools;
    if (code_execution_enabled) native_tools.push_back("code_execution");
    if (google_search_enabled) native_tools.push_back("google_search");
    if (open_ai_web_search_enabled) native_tools.push_back("web_search");
    for (const std::string& native_tool : native_tools) {
        tool_cost += costs_.envelope.per_tool_definition + text_raw(costs_, native_tool);
    }
    if (!tools.empty() || !native_tools.empty()) {
        tool_cost += costs_.envelope.tool_set_overhead;
    }
    return costs_.safety_margin.apply(
        coerce_at_most_int_max(prompt) + coerce_at_most_int_max(tool_cost));
}

FixedContextComposition CostModelContextEstimator::estimate_fixed_composition(
    const std::optional<std::string>& system_prompt,
    const std::vector<openai::ToolDefinition>& tools,
    const std::optional<std::string>& initial_user_prompt, bool code_execution_enabled,
    bool google_search_enabled, bool open_ai_web_search_enabled) const {
    // The margin is applied per part here, so the parts can differ from estimateFixed by a token.
    std::int64_t prompt = costs_.envelope.request_overhead + costs_.envelope.per_message +
                          text_raw(costs_, system_prompt.value_or(""));
    if (initial_user_prompt && !json::is_blank(*initial_user_prompt)) {
        prompt += costs_.envelope.per_message + text_raw(costs_, *initial_user_prompt);
    }
    std::int64_t tool_cost = 0;
    for (const openai::ToolDefinition& tool : tools) {
        tool_cost += costs_.envelope.per_tool_definition;
        tool_cost += text_raw(costs_, tool.type);
        tool_cost += text_raw(costs_, tool.function.name);
        tool_cost += text_raw(costs_, tool.function.description);
        if (tool.function.parameters.schema) {
            tool_cost += text_raw(costs_, json::to_kotlin_string(*tool.function.parameters.schema));
        } else {
            tool_cost += text_raw(costs_, tool.function.parameters.type);
            std::vector<std::pair<std::string, openai::ToolProperty>> sorted_properties =
                tool.function.parameters.properties;
            std::stable_sort(sorted_properties.begin(), sorted_properties.end(),
                             [](const auto& left, const auto& right) {
                                 return left.first < right.first;
                             });
            for (const auto& [name, property] : sorted_properties) {
                tool_cost += text_raw(costs_, name);
                tool_cost += tool_property_raw(costs_, property);
            }
            std::vector<std::string> sorted_required = tool.function.parameters.required;
            std::sort(sorted_required.begin(), sorted_required.end());
            for (const std::string& required : sorted_required) {
                tool_cost += text_raw(costs_, required);
            }
        }
    }
    std::vector<std::string> native_tools;
    if (code_execution_enabled) native_tools.push_back("code_execution");
    if (google_search_enabled) native_tools.push_back("google_search");
    if (open_ai_web_search_enabled) native_tools.push_back("web_search");
    for (const std::string& native_tool : native_tools) {
        tool_cost += costs_.envelope.per_tool_definition + text_raw(costs_, native_tool);
    }
    if (!tools.empty() || !native_tools.empty()) {
        tool_cost += costs_.envelope.tool_set_overhead;
    }
    return FixedContextComposition{
        costs_.safety_margin.apply(coerce_at_most_int_max(prompt)),
        costs_.safety_margin.apply(coerce_at_most_int_max(tool_cost)),
    };
}

namespace {

std::int64_t tool_property_raw(const ContextCostModel& costs, const openai::ToolProperty& property) {
    // A property is priced with everything it nests: the whole schema is what the provider gets.
    std::int64_t total = text_raw(costs, property.type) + text_raw(costs, property.description);
    if (property.items) total += tool_property_raw(costs, *property.items);
    if (property.properties) {
        for (const auto& [name, nested] : *property.properties) {
            total += text_raw(costs, name) + tool_property_raw(costs, nested);
        }
    }
    if (property.required) {
        for (const std::string& required : *property.required) {
            total += text_raw(costs, required);
        }
    }
    return total;
}

}  // namespace

}  // namespace agora::core::prep
