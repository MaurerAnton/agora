#pragma once

#include <optional>
#include <string>
#include <vector>

namespace agora::core::openai_service_tiers {

// Port of model/OpenAiServiceTiers.kt: the OpenAI service-tier policy shared by settings
// persistence (normalize on write) and request building (requestValue). The tier strings are
// part of the wire contract, so the port keeps them as strings instead of an enum.

/** OpenAiServiceTiers.values, in UI order. */
const std::vector<std::string>& values();

/** OpenAiServiceTiers.normalize: trim, lowercase, map the legacy aliases, fall back to auto. */
std::string normalize(const std::optional<std::string>& value);

/** OpenAiServiceTiers.availableTiers: which tiers the model can actually serve. */
std::vector<std::string> available_tiers(const std::optional<std::string>& model_id,
                                         bool official_provider = true);

/** OpenAiServiceTiers.mappedTier: the closest supported tier, defaulting hard to "default". */
std::string mapped_tier(const std::optional<std::string>& value,
                        const std::optional<std::string>& model_id,
                        bool official_provider = true);

/** OpenAiServiceTiers.indexForTier: index into values(); normalize never misses. */
int index_for_tier(const std::optional<std::string>& value);

/** OpenAiServiceTiers.tierForIndex: clamps to the values() range like coerceIn(indices). */
std::string tier_for_index(int index);

/** OpenAiServiceTiers.requestValue: the wire value, present only when enabled and supported by
 *  the Responses API. */
std::optional<std::string> request_value(bool enabled,
                                         const std::optional<std::string>& value,
                                         bool responses_api_enabled,
                                         const std::optional<std::string>& model_id = std::nullopt,
                                         bool official_provider = true);

}  // namespace agora::core::openai_service_tiers
