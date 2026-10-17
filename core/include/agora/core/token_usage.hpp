#pragma once

#include <cstdint>
#include <limits>
#include <optional>

namespace agora::core {

/**
 * model/TokenUsage.kt — the full token-usage record stream events carry. Shared by the stream
 * router modules (anthropic-stream, gemini-stream, ollama) so the shape exists once.
 */
struct TokenUsage {
    std::int32_t total_token_count = 0;
    std::optional<std::int32_t> input_token_count;
    std::optional<std::int32_t> cached_input_token_count;
    std::optional<std::int32_t> cache_write_input_token_count;
    std::optional<std::int32_t> uncached_input_token_count;
    std::optional<std::int32_t> output_token_count;
    std::optional<std::int32_t> reasoning_token_count;
};

/** TokenUsage.addCounts: saturating Int addition. */
inline std::int32_t add_token_counts(std::int32_t left, std::int32_t right) {
    const std::int64_t sum = static_cast<std::int64_t>(left) + static_cast<std::int64_t>(right);
    if (sum > std::numeric_limits<std::int32_t>::max()) {
        return std::numeric_limits<std::int32_t>::max();
    }
    return static_cast<std::int32_t>(sum);
}

}  // namespace agora::core
