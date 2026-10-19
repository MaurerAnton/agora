#include "agora/core/token_pricing.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <limits>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/unicode.hpp"

namespace agora::core::token_pricing {
namespace {

bool is_ascii_digit(char c) { return c >= '0' && c <= '9'; }

bool is_ascii_alnum(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z');
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string after_last_slash(std::string_view text) {
    const std::size_t slash = text.find_last_of('/');
    if (slash == std::string_view::npos) return std::string(text);
    return std::string(text.substr(slash + 1));
}

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool contains(std::string_view text, std::string_view needle) {
    return text.find(needle) != std::string_view::npos;
}

int digits_value(std::string_view text, std::size_t offset, int count) {
    int value = 0;
    for (int index = 0; index < count; ++index) {
        value = value * 10 + (text[offset + static_cast<std::size_t>(index)] - '0');
    }
    return value;
}

/** Leftmost VERSION_PAIR match: (?<!\d)(\d{1,2})[-.](\d{1,2})(?!\d). */
std::optional<std::pair<int, int>> find_version_pair(std::string_view name) {
    for (std::size_t start = 0; start < name.size(); ++start) {
        if (!is_ascii_digit(name[start])) continue;
        if (start > 0 && is_ascii_digit(name[start - 1])) continue;
        for (int major_length = 2; major_length >= 1; --major_length) {
            const std::size_t major_end = start + static_cast<std::size_t>(major_length);
            if (major_end > name.size()) continue;
            bool digits = true;
            for (int index = 0; index < major_length; ++index) {
                if (!is_ascii_digit(name[start + static_cast<std::size_t>(index)])) {
                    digits = false;
                    break;
                }
            }
            if (!digits) continue;
            if (major_end >= name.size()) continue;
            const char delimiter = name[major_end];
            if (delimiter != '-' && delimiter != '.') continue;
            const std::size_t minor_start = major_end + 1;
            for (int minor_length = 2; minor_length >= 1; --minor_length) {
                const std::size_t minor_end = minor_start + static_cast<std::size_t>(minor_length);
                if (minor_end > name.size()) continue;
                bool minor_digits = true;
                for (int index = 0; index < minor_length; ++index) {
                    if (!is_ascii_digit(name[minor_start + static_cast<std::size_t>(index)])) {
                        minor_digits = false;
                        break;
                    }
                }
                if (!minor_digits) continue;
                if (minor_end < name.size() && is_ascii_digit(name[minor_end])) continue;
                return std::make_pair(digits_value(name, start, major_length),
                                      digits_value(name, minor_start, minor_length));
            }
        }
    }
    return std::nullopt;
}

/** Leftmost MAJOR_ONLY match: (?<![\d.])(\d{1,2})(?![\d.]). */
std::optional<int> find_major_only(std::string_view name) {
    for (std::size_t start = 0; start < name.size(); ++start) {
        if (!is_ascii_digit(name[start])) continue;
        if (start > 0 && (is_ascii_digit(name[start - 1]) || name[start - 1] == '.')) continue;
        for (int length = 2; length >= 1; --length) {
            const std::size_t end = start + static_cast<std::size_t>(length);
            if (end > name.size()) continue;
            bool digits = true;
            for (int index = 0; index < length; ++index) {
                if (!is_ascii_digit(name[start + static_cast<std::size_t>(index)])) {
                    digits = false;
                    break;
                }
            }
            if (!digits) continue;
            if (end < name.size()) {
                const char next = name[end];
                if (is_ascii_digit(next) || next == '.') continue;
            }
            return digits_value(name, start, length);
        }
    }
    return std::nullopt;
}

std::int64_t anthropic_visual_tokens(int width, int height, int scaled_long_edge) {
    const int long_edge = std::max(width, height);
    const int short_edge = std::min(width, height);
    const int scaled_short_edge = proportional_edge(short_edge, long_edge, scaled_long_edge);
    return blocks_covering(scaled_long_edge, 28) * blocks_covering(scaled_short_edge, 28);
}

template <typename Function>
std::int64_t with_pixels(const ImageDescriptor& image, Function function) {
    if (image.pixel_width.has_value() && *image.pixel_width > 0 &&
        image.pixel_height.has_value() && *image.pixel_height > 0) {
        return function(*image.pixel_width, *image.pixel_height);
    }
    return byte_proportional_tokens(image);
}

std::int64_t patches_covering(int width, int height) {
    return blocks_covering(width, 32) * blocks_covering(height, 32);
}

double shrink_factor(int width, int height, int budget) {
    const double area = static_cast<double>(width) * height;
    const double by_area = std::sqrt(32.0 * 32.0 * budget / area);
    const double width_patches = width * by_area / 32.0;
    const double height_patches = height * by_area / 32.0;
    const double correction =
        std::min(std::floor(width_patches) / width_patches,
                 std::floor(height_patches) / height_patches);
    return by_area * correction;
}

std::int64_t patch_image_tokens(int width, int height, int max_edge_px,
                                std::optional<int> patch_budget, double multiplier) {
    const int long_edge = std::max(width, height);
    int fitted_width = width;
    int fitted_height = height;
    if (long_edge > max_edge_px) {
        if (width >= height) {
            fitted_width = max_edge_px;
            fitted_height = proportional_edge(height, width, max_edge_px);
        } else {
            fitted_height = max_edge_px;
            fitted_width = proportional_edge(width, height, max_edge_px);
        }
    }
    std::int64_t patches = patches_covering(fitted_width, fitted_height);
    if (patch_budget.has_value() && patches > *patch_budget) {
        const double shrink = shrink_factor(fitted_width, fitted_height, *patch_budget);
        int scaled_width = static_cast<int>(std::floor(fitted_width * shrink));
        int scaled_height = static_cast<int>(std::floor(fitted_height * shrink));
        if (scaled_width < 1) scaled_width = 1;
        if (scaled_height < 1) scaled_height = 1;
        patches = patches_covering(scaled_width, scaled_height);
    }
    return static_cast<std::int64_t>(std::ceil(static_cast<double>(patches) * multiplier));
}

std::int64_t tile_image_tokens(int width, int height, int base_tokens, int tile_tokens) {
    constexpr int kMaxSquarePx = 2048;
    constexpr int kShortEdgePx = 768;
    constexpr int kTilePx = 512;

    int fitted_width = width;
    int fitted_height = height;
    const int long_edge = std::max(width, height);
    if (long_edge > kMaxSquarePx) {
        if (width >= height) {
            fitted_width = kMaxSquarePx;
            fitted_height = proportional_edge(height, width, kMaxSquarePx);
        } else {
            fitted_height = kMaxSquarePx;
            fitted_width = proportional_edge(width, height, kMaxSquarePx);
        }
    }
    const int short_edge = std::min(fitted_width, fitted_height);
    if (short_edge > kShortEdgePx) {
        const double ratio = static_cast<double>(kShortEdgePx) / short_edge;
        if (fitted_width <= fitted_height) {
            fitted_height = static_cast<int>(std::floor(fitted_height * ratio));
            if (fitted_height < 1) fitted_height = 1;
            fitted_width = kShortEdgePx;
        } else {
            fitted_width = static_cast<int>(std::floor(fitted_width * ratio));
            if (fitted_width < 1) fitted_width = 1;
            fitted_height = kShortEdgePx;
        }
    }
    const std::int64_t tiles =
        blocks_covering(fitted_width, kTilePx) * blocks_covering(fitted_height, kTilePx);
    return base_tokens + tiles * tile_tokens;
}

struct EnvelopeEntry {
    AnthropicModelId::Tier tier;
    int version;
    int overhead;
};

const std::vector<EnvelopeEntry>& envelope_table() {
    static const std::vector<EnvelopeEntry> kEntries = {
        {AnthropicModelId::Tier::Opus, 55, 286},
        {AnthropicModelId::Tier::Opus, 50, 286},
        {AnthropicModelId::Tier::Opus, 48, 290},
        {AnthropicModelId::Tier::Opus, 47, 675},
        {AnthropicModelId::Tier::Opus, 46, 497},
        {AnthropicModelId::Tier::Opus, 45, 496},
        {AnthropicModelId::Tier::Opus, 41, 313},
        {AnthropicModelId::Tier::Opus, 40, 313},
        {AnthropicModelId::Tier::Sonnet, 50, 354},
        {AnthropicModelId::Tier::Sonnet, 46, 497},
        {AnthropicModelId::Tier::Sonnet, 45, 496},
        {AnthropicModelId::Tier::Sonnet, 40, 313},
        {AnthropicModelId::Tier::Haiku, 45, 496},
        {AnthropicModelId::Tier::Haiku, 35, 264},
    };
    return kEntries;
}

constexpr int kLargestPublishedOverhead = 675;

std::string tier_text(AnthropicModelId::Tier tier) {
    switch (tier) {
        case AnthropicModelId::Tier::Opus: return "OPUS";
        case AnthropicModelId::Tier::Sonnet: return "SONNET";
        case AnthropicModelId::Tier::Haiku: return "HAIKU";
    }
    return "OPUS";
}

std::string optional_int_text(const std::optional<int>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

std::string optional_long_text(const std::optional<std::int64_t>& value) {
    return value.has_value() ? std::to_string(*value) : "null";
}

}  // namespace

std::int64_t byte_proportional_tokens(const ImageDescriptor& image) {
    if (!image.byte_size.has_value() || *image.byte_size <= 0) return kUnknownImageTokens;
    const std::int64_t tokens = *image.byte_size / kBytesPerToken;
    if (tokens < kMinImageTokens) return kMinImageTokens;
    if (tokens > kMaxImageTokens) return kMaxImageTokens;
    return tokens;
}

std::int64_t blocks_covering(int value, int divisor) {
    if (divisor <= 0) return 0;
    if (value <= 0) return 0;
    return (static_cast<std::int64_t>(value) + divisor - 1) / divisor;
}

int proportional_edge(int edge, int from_edge, int to_edge) {
    if (from_edge <= 0) return 1;
    const double scaled = static_cast<double>(edge) * to_edge / from_edge;
    int rounded = static_cast<int>(std::floor(scaled + 0.5));
    if (rounded < 1) rounded = 1;
    return rounded;
}

bool AnthropicModelId::version_at_least(int other_major, int other_minor) const {
    if (!major.has_value()) return false;
    return *major > other_major || (*major == other_major && minor >= other_minor);
}

AnthropicModelId AnthropicModelId::parse(std::string_view model_name) {
    const std::string name = ascii_lower(after_last_slash(model_name));
    std::optional<Tier> tier;
    if (contains(name, "opus")) {
        tier = Tier::Opus;
    } else if (contains(name, "sonnet")) {
        tier = Tier::Sonnet;
    } else if (contains(name, "haiku")) {
        tier = Tier::Haiku;
    }
    if (const std::optional<std::pair<int, int>> pair = find_version_pair(name)) {
        return AnthropicModelId{tier, pair->first, pair->second};
    }
    return AnthropicModelId{tier, find_major_only(name), 0};
}

AnthropicImageTier anthropic_tier_for_model_name(std::string_view model_name) {
    return AnthropicModelId::parse(model_name).version_at_least(4, 7)
               ? AnthropicImageTier::HighResolution
               : AnthropicImageTier::Standard;
}

std::int64_t anthropic_image_tokens(AnthropicImageTier tier, const ImageDescriptor& image) {
    const int max_long_edge_px = tier == AnthropicImageTier::Standard ? 1568 : 2576;
    const std::int64_t max_visual_tokens = tier == AnthropicImageTier::Standard ? 1568 : 4784;
    return with_pixels(image, [&](int width, int height) {
        const int long_edge = std::max(width, height);
        int low = 1;
        int high = std::min(long_edge, max_long_edge_px);
        int best = 1;
        while (low <= high) {
            const int middle = low + (high - low) / 2;
            if (anthropic_visual_tokens(width, height, middle) <= max_visual_tokens) {
                best = middle;
                low = middle + 1;
            } else {
                high = middle - 1;
            }
        }
        return anthropic_visual_tokens(width, height, best);
    });
}

std::int64_t gemini_image_tokens(const ImageDescriptor& image) {
    constexpr std::int64_t kTokensPerTile = 258;
    constexpr int kSingleTileMaxEdgePx = 384;
    return with_pixels(image, [](int width, int height) {
        if (width <= kSingleTileMaxEdgePx && height <= kSingleTileMaxEdgePx) {
            return kTokensPerTile;
        }
        int crop_unit = static_cast<int>(
            std::floor(static_cast<double>(std::min(width, height)) / 1.5));
        if (crop_unit < 1) crop_unit = 1;
        const std::int64_t tiles =
            blocks_covering(width, crop_unit) * blocks_covering(height, crop_unit);
        const std::int64_t tokens = tiles * kTokensPerTile;
        return tokens < kTokensPerTile ? kTokensPerTile : tokens;
    });
}

std::int64_t openai_image_tokens(std::string_view model_name, const ImageDescriptor& image) {
    const std::string name = ascii_lower(after_last_slash(model_name));
    return with_pixels(image, [&](int width, int height) {
        if (starts_with(name, "gpt-4o-mini")) return tile_image_tokens(width, height, 2833, 5667);
        if (starts_with(name, "gpt-4o")) return tile_image_tokens(width, height, 85, 170);
        if (starts_with(name, "gpt-4.1-mini")) {
            return patch_image_tokens(width, height, 2048, 6144, 1.62);
        }
        if (starts_with(name, "gpt-4.1-nano")) {
            return patch_image_tokens(width, height, 2048, 6144, 2.46);
        }
        if (starts_with(name, "gpt-4.1")) return tile_image_tokens(width, height, 85, 170);
        if (starts_with(name, "gpt-5.1")) return tile_image_tokens(width, height, 70, 140);
        if (starts_with(name, "gpt-5.2")) {
            return patch_image_tokens(width, height, 2048, 6144, 1.2);
        }
        if (starts_with(name, "gpt-5.4")) {
            return patch_image_tokens(width, height, 2048, 2500, 1.2);
        }
        if (starts_with(name, "gpt-5.5")) {
            return patch_image_tokens(width, height, 6000, 10000, 1.2);
        }
        if (starts_with(name, "gpt-5-nano")) {
            return patch_image_tokens(width, height, 2048, 2500, 1.5);
        }
        if (starts_with(name, "gpt-5-mini")) {
            return patch_image_tokens(width, height, 2048, 2500, 1.2);
        }
        if (starts_with(name, "gpt-5")) return tile_image_tokens(width, height, 70, 140);
        if (starts_with(name, "o4-mini")) {
            return patch_image_tokens(width, height, 2048, 2500, 1.72);
        }
        if (starts_with(name, "o1") || starts_with(name, "o3")) {
            return tile_image_tokens(width, height, 75, 150);
        }
        return patch_image_tokens(width, height, 65535, std::nullopt, 1.2);
    });
}

EnvelopeCost default_envelope() { return EnvelopeCost{8, 16, 16, 0, 0}; }

EnvelopeCost openai_envelope() { return EnvelopeCost{3, 16, 16, 3, 0}; }

EnvelopeCost anthropic_envelope(std::string_view model_name) {
    const AnthropicModelId model = AnthropicModelId::parse(model_name);
    int overhead = kLargestPublishedOverhead;
    if (model.tier.has_value() && model.major.has_value()) {
        const int version = *model.major * 10 + model.minor;
        for (const EnvelopeEntry& entry : envelope_table()) {
            if (entry.tier == *model.tier && entry.version == version) {
                overhead = entry.overhead;
                break;
            }
        }
    }
    EnvelopeCost envelope = default_envelope();
    envelope.tool_set_overhead = overhead;
    return envelope;
}

int SafetyMargin::apply(std::int64_t raw) const {
    std::int64_t value = (raw * numerator + denominator - 1) / denominator;
    if (value < 0) value = 0;
    if (value > std::numeric_limits<int>::max()) value = std::numeric_limits<int>::max();
    return static_cast<int>(value);
}

SafetyMargin default_safety_margin() { return SafetyMargin{11, 10}; }

std::int64_t heuristic_text_tokens(std::string_view text) {
    std::int64_t tokens = 0;
    int ascii_run = 0;
    const auto flush = [&]() {
        if (ascii_run > 0) {
            tokens += (ascii_run + 3) / 4;
            ascii_run = 0;
        }
    };
    std::size_t offset = 0;
    while (offset < text.size()) {
        const unicode::Decoded decoded = unicode::decode_utf8(text.data() + offset,
                                                             text.size() - offset);
        const std::uint32_t code_point = decoded.code_point;
        if (code_point <= 0x7f && is_ascii_alnum(static_cast<char>(code_point))) {
            ++ascii_run;
        } else if (unicode::is_jvm_whitespace(code_point)) {
            flush();
        } else {
            flush();
            ++tokens;
        }
        offset += decoded.length;
    }
    flush();
    return tokens;
}

std::int64_t local_model_text_tokens(std::string_view text, std::optional<int> exact_count,
                                     bool exact_throws) {
    if (text.empty()) return 0;
    if (!exact_throws && exact_count.has_value() && *exact_count >= 0) {
        return *exact_count;
    }
    return heuristic_text_tokens(text);
}

ContextCostModel default_context_cost_model() {
    return ContextCostModel{&heuristic_text_tokens, &byte_proportional_tokens, default_envelope(),
                            default_safety_margin()};
}

std::string describe(const ImageDescriptor& image) {
    return "ImageDescriptor(byteSize=" + optional_long_text(image.byte_size) +
           ", pixelWidth=" + optional_int_text(image.pixel_width) +
           ", pixelHeight=" + optional_int_text(image.pixel_height) + ")";
}

std::string describe(const AnthropicModelId& model) {
    return "AnthropicModelId(tier=" +
           (model.tier.has_value() ? tier_text(*model.tier) : std::string("null")) +
           ", major=" + optional_int_text(model.major) +
           ", minor=" + std::to_string(model.minor) + ")";
}

std::string describe(AnthropicImageTier tier) {
    return tier == AnthropicImageTier::Standard ? "STANDARD" : "HIGH_RESOLUTION";
}

std::string describe(const EnvelopeCost& envelope) {
    return "EnvelopeCost(perMessage=" + std::to_string(envelope.per_message) +
           ", perToolDefinition=" + std::to_string(envelope.per_tool_definition) +
           ", perToolCall=" + std::to_string(envelope.per_tool_call) +
           ", requestOverhead=" + std::to_string(envelope.request_overhead) +
           ", toolSetOverhead=" + std::to_string(envelope.tool_set_overhead) + ")";
}

}  // namespace agora::core::token_pricing
