#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace agora::core::loop_policy {

/**
 * Port of automation/LoopPolicy.kt: the cost and cadence guardrails shared by the Loop domain layer
 * and the model-facing tools.
 *
 * The Kotlin object has no imports and no state, so this is a straight transcription of the
 * constants and the five pure functions. The one thing that needs care is signed overflow: Kotlin's
 * Long arithmetic wraps, C++'s is undefined, and next_fire_at's own overflow guard overflows when
 * the interval is negative.
 */

inline constexpr std::int64_t kMinIntervalSeconds = 10;
inline constexpr std::int64_t kMaxIntervalSeconds = 604'800;  // 7 days
inline constexpr std::int64_t kMinIntervalMs = kMinIntervalSeconds * 1'000;
inline constexpr std::int64_t kMaxIntervalMs = kMaxIntervalSeconds * 1'000;

inline constexpr int kDefaultMaxCycles = 10;
inline constexpr int kMinMaxCycles = 1;
inline constexpr int kMaxMaxCycles = 100;

inline constexpr std::string_view kDefaultPrompt = "Continue.";

/** The validation message, or nullopt when the pair is acceptable. */
std::optional<std::string> validate(std::int64_t interval_ms, int max_cycles);

/** The trimmed prompt, or nullopt when it is blank. */
std::optional<std::string> normalize_prompt(std::optional<std::string_view> prompt);

/** The prompt an execution actually uses: the trimmed prompt, or the default. */
std::string prompt_for_execution(std::optional<std::string_view> prompt);

/** `now + interval_ms`, saturated at Long.MAX_VALUE on overflow. */
std::int64_t next_fire_at(std::int64_t now, std::int64_t interval_ms);

/** The next revision counter, wrapping back to zero at Long.MAX_VALUE. */
std::int64_t next_revision(std::int64_t current);

}  // namespace agora::core::loop_policy
