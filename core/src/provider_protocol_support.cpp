#include "agora/core/provider_protocol_support.hpp"

#include <algorithm>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::provider_protocol_support {
namespace {

const CustomProviderConfig* first_match(
    std::string_view provider_name, const std::vector<CustomProviderConfig>& custom_providers) {
    for (const CustomProviderConfig& provider : custom_providers) {
        if (provider.name == provider_name ||
            model_thinking_resolver::owns_identity(provider, provider_name)) {
            return &provider;
        }
    }
    return nullptr;
}

bool matches(std::string_view provider_name, const CustomProviderConfig& provider) {
    return provider.name == provider_name ||
           model_thinking_resolver::owns_identity(provider, provider_name);
}

}  // namespace

bool is_open_ai_protocol_provider(std::string_view provider_name,
                                  const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kOpenAiProvider) return true;
    return std::any_of(custom_providers.begin(), custom_providers.end(),
                       [&](const CustomProviderConfig& provider) {
                           return matches(provider_name, provider) &&
                                  provider.protocol == CustomEndpointProtocol::openai;
                       });
}

bool is_anthropic_protocol_provider(std::string_view provider_name,
                                    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kAnthropicProvider) return true;
    return std::any_of(custom_providers.begin(), custom_providers.end(),
                       [&](const CustomProviderConfig& provider) {
                           return matches(provider_name, provider) &&
                                  provider.protocol == CustomEndpointProtocol::anthropic;
                       });
}

bool is_anthropic_cache_enabled_for_provider(
    std::string_view provider_name, bool built_in_enabled,
    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kAnthropicProvider) return built_in_enabled;
    const CustomProviderConfig* provider = first_match(provider_name, custom_providers);
    return provider != nullptr && provider->protocol == CustomEndpointProtocol::anthropic &&
           provider->anthropic_cache_enabled;
}

bool supports_prompt_cache_setting(std::string_view provider_name,
                                   const std::vector<CustomProviderConfig>& custom_providers) {
    if (is_anthropic_protocol_provider(provider_name, custom_providers)) return true;
    return std::any_of(custom_providers.begin(), custom_providers.end(),
                       [&](const CustomProviderConfig& provider) {
                           return matches(provider_name, provider) &&
                                  provider.protocol == CustomEndpointProtocol::openai;
                       });
}

bool is_prompt_cache_key_enabled_for_provider(
    std::string_view provider_name,
    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kOpenAiProvider) return true;
    const CustomProviderConfig* provider = first_match(provider_name, custom_providers);
    return provider != nullptr && provider->protocol == CustomEndpointProtocol::openai &&
           provider->anthropic_cache_enabled;
}

bool is_prompt_cache_setting_enabled_for_provider(
    std::string_view provider_name, bool built_in_anthropic_enabled,
    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kAnthropicProvider) return built_in_anthropic_enabled;
    const CustomProviderConfig* provider = first_match(provider_name, custom_providers);
    return provider != nullptr ? provider->anthropic_cache_enabled : true;
}

std::string anthropic_cache_ttl_for_provider(
    std::string_view provider_name, std::string_view built_in_ttl,
    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kAnthropicProvider) return std::string(built_in_ttl);
    const CustomProviderConfig* provider = first_match(provider_name, custom_providers);
    return provider != nullptr ? provider->anthropic_cache_ttl : std::string("1h");
}

bool is_responses_api_enabled_for_provider(
    std::string_view provider_name, bool built_in_open_ai_enabled,
    const std::vector<CustomProviderConfig>& custom_providers) {
    if (provider_name == kOpenAiProvider) return built_in_open_ai_enabled;
    const CustomProviderConfig* provider = first_match(provider_name, custom_providers);
    return provider != nullptr && provider->protocol == CustomEndpointProtocol::openai &&
           provider->responses_api_enabled;
}

}  // namespace agora::core::provider_protocol_support
