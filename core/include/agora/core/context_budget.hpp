#pragma once

#include <optional>
#include <string>
#include <vector>

namespace agora::core::context_budget {

// Port of model/ContextBudget.kt: shared storage/UI bounds for the provider-visible conversation
// token budget, plus the legacy-message-window migration applied at every read/use boundary.

inline constexpr int kDefaultTokens = 262144;
inline constexpr int kMinTokens = 4096;
inline constexpr int kMaxTokens = 1048576;

/** ContextBudget.PRESETS, ascending. */
const std::vector<int>& presets();

/**
 * ContextBudget.normalize: null/<=0 -> default, values <= 100 are legacy logical-message windows
 * multiplied by 1024, then clamped to [MIN_TOKENS, MAX_TOKENS].
 */
int normalize(std::optional<int> value);

/**
 * ContextBudget.compactLabel: compact label for configured budgets and live occupancy. Values
 * below 101 stay decimal here — applying the legacy migration in a label would turn an empty
 * context into "32K".
 */
std::string compact_label(int tokens);

}  // namespace agora::core::context_budget
