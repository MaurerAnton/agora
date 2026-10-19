#pragma once

#include <cstdint>
#include <optional>
#include <string_view>

namespace agora::core::token_pricing {

/**
 * Port of LocalModelTextCounter.kt — the one file of api/util/tokens that the
 * message-preparation slice (message_preparation_tokens.cpp) deliberately leaves to Kotlin: exact
 * counting for the embedded model with the offline heuristic as a fallback.
 *
 * Everything else in the pricing cluster lives in agora::core::prep (see
 * message_preparation.hpp) and is pinned by BOTH the message-preparation stand and the wider
 * token-pricing stand (core/tools/golden/token-pricing, 154 traces).
 *
 * `exact_count` is the runtime's answer: null when no model is resident, negative when the
 * runtime reports failure. `exact_throws` mirrors Kotlin's runCatching around the callback.
 */
std::int64_t local_model_text_tokens(std::string_view text, std::optional<int> exact_count,
                                     bool exact_throws);

}  // namespace agora::core::token_pricing
