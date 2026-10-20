#pragma once

#include <string>
#include <string_view>
#include <vector>

#include "agora/core/model_thinking_resolver.hpp"

namespace agora::core::provider_protocol_support {

/**
 * Port of the provider-protocol helpers in data/CustomProviderConfig.kt.
 *
 * A custom provider's wire protocol decides request behaviour; its display name is only a user
 * label. Custom providers are searched in list order and the first name/id/legacy match wins,
 * exactly like the Kotlin firstOrNull calls.
 */

using CustomProviderConfig = model_thinking_resolver::CustomProviderConfig;
using CustomEndpointProtocol = model_thinking_resolver::CustomEndpointProtocol;

/** Constants.PROVIDER_OPENAI / PROVIDER_ANTHROPIC used by the Kotlin when branches. */
inline constexpr std::string_view kOpenAiProvider = "OpenAI";
inline constexpr std::string_view kAnthropicProvider = "Anthropic";

/** isOpenAiProtocolProvider: the built-in name or a matching custom OpenAI endpoint. */
bool is_open_ai_protocol_provider(std::string_view provider_name,
                                  const std::vector<CustomProviderConfig>& custom_providers);

/** isAnthropicProtocolProvider. */
bool is_anthropic_protocol_provider(std::string_view provider_name,
                                    const std::vector<CustomProviderConfig>& custom_providers);

/** isAnthropicCacheEnabledForProvider: built-in value, else the matched custom cache switch. */
bool is_anthropic_cache_enabled_for_provider(
    std::string_view provider_name, bool built_in_enabled,
    const std::vector<CustomProviderConfig>& custom_providers);

/** supportsPromptCacheSetting. */
bool supports_prompt_cache_setting(std::string_view provider_name,
                                   const std::vector<CustomProviderConfig>& custom_providers);

/** isPromptCacheKeyEnabledForProvider. */
bool is_prompt_cache_key_enabled_for_provider(
    std::string_view provider_name,
    const std::vector<CustomProviderConfig>& custom_providers);

/** isPromptCacheSettingEnabledForProvider. */
bool is_prompt_cache_setting_enabled_for_provider(
    std::string_view provider_name, bool built_in_anthropic_enabled,
    const std::vector<CustomProviderConfig>& custom_providers);

/** anthropicCacheTtlForProvider: built-in TTL, else the matched custom TTL or "1h". */
std::string anthropic_cache_ttl_for_provider(
    std::string_view provider_name, std::string_view built_in_ttl,
    const std::vector<CustomProviderConfig>& custom_providers);

/** isResponsesApiEnabledForProvider. */
bool is_responses_api_enabled_for_provider(
    std::string_view provider_name, bool built_in_open_ai_enabled,
    const std::vector<CustomProviderConfig>& custom_providers);

}  // namespace agora::core::provider_protocol_support
