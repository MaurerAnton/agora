#include "agora/core/loop_policy.hpp"

#include <limits>
#include <string>

#include "agora/core/json.hpp"

namespace agora::core::loop_policy {
namespace {

constexpr std::int64_t kLongMax = std::numeric_limits<std::int64_t>::max();

/** Two's-complement addition without invoking signed overflow, which C++ leaves undefined. */
std::int64_t wrapping_add(std::int64_t left, std::int64_t right) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(left) +
                                     static_cast<std::uint64_t>(right));
}

/** Two's-complement subtraction, for the same reason. */
std::int64_t wrapping_sub(std::int64_t left, std::int64_t right) {
    return static_cast<std::int64_t>(static_cast<std::uint64_t>(left) -
                                     static_cast<std::uint64_t>(right));
}

}  // namespace

std::optional<std::string> validate(std::int64_t interval_ms, int max_cycles) {
    if (interval_ms < kMinIntervalMs || interval_ms > kMaxIntervalMs) {
        return "interval must be between " + std::to_string(kMinIntervalSeconds) + " and " +
               std::to_string(kMaxIntervalSeconds) + " seconds";
    }
    if (max_cycles < kMinMaxCycles || max_cycles > kMaxMaxCycles) {
        return "maxCycles must be between " + std::to_string(kMinMaxCycles) + " and " +
               std::to_string(kMaxMaxCycles);
    }
    return std::nullopt;
}

std::optional<std::string> normalize_prompt(std::optional<std::string_view> prompt) {
    if (!prompt.has_value()) return std::nullopt;
    std::string trimmed = json::trim(*prompt);
    if (trimmed.empty()) return std::nullopt;
    return trimmed;
}

std::string prompt_for_execution(std::optional<std::string_view> prompt) {
    const std::optional<std::string> normalized = normalize_prompt(prompt);
    return normalized.has_value() ? *normalized : std::string(kDefaultPrompt);
}

std::int64_t next_fire_at(std::int64_t now, std::int64_t interval_ms) {
    // Kotlin: if (now > Long.MAX_VALUE - intervalMs) Long.MAX_VALUE else now + intervalMs.
    // The guard subtraction wraps for a negative interval, so replicate it bit for bit rather than
    // "fixing" it -- a schedule with a negative interval must clamp here, not in a later version.
    const std::int64_t threshold = wrapping_sub(kLongMax, interval_ms);
    if (now > threshold) return kLongMax;
    return wrapping_add(now, interval_ms);
}

std::int64_t next_revision(std::int64_t current) {
    return current == kLongMax ? 0 : current + 1;
}

}  // namespace agora::core::loop_policy
