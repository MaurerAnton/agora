#include "agora/core/token_pricing.hpp"

#include <optional>
#include <string_view>

#include "agora/core/text_counter.hpp"

namespace agora::core::token_pricing {

std::int64_t local_model_text_tokens(std::string_view text, std::optional<int> exact_count,
                                     bool exact_throws) {
    // Empty text never consults the runtime: the indicator shows 0 before any model is resident.
    if (text.empty()) return 0;
    // A thrown answer or a negative count is a failure report, not a cost.
    if (!exact_throws && exact_count.has_value() && *exact_count >= 0) return *exact_count;
    return HeuristicTextCounter::instance().count(text);
}

}  // namespace agora::core::token_pricing
