#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/thinking_capability.hpp"

namespace agora::core::model_thinking_resolver {

/**
 * Port of model/ModelId.kt: a typed `ProviderName:modelId` pair with the legacy unprefixed
 * heuristics used by ProviderRegistry.
 */
struct ModelId {
    std::string provider_name;
    std::string model_name;

    std::string prefixed() const { return provider_name + ":" + model_name; }
    std::string api_model_name() const;
};

/** ModelId.parse: split at the first colon, otherwise infer the provider from the model name. */
ModelId parse_model_id(std::string_view prefixed);

/** Port of data/CustomProviderConfig.kt's CustomEndpointProtocol and its wire values. */
enum class CustomEndpointProtocol {
    openai,
    google,
    anthropic,
    unknown,
};

std::string_view wire_value(CustomEndpointProtocol protocol);
CustomEndpointProtocol protocol_from_wire_value(std::string_view value);

/**
 * The fields of CustomProviderConfig that participate in identity and thinking resolution.
 * `legacy_names` is an ordered set in Kotlin; membership is the only observable operation here.
 */
struct CustomProviderConfig {
    std::string name;
    CustomEndpointProtocol protocol = CustomEndpointProtocol::openai;
    std::string id;
    std::vector<std::string> legacy_names;
    /** Retained across protocol changes; consumed only by OpenAI-protocol requests. */
    bool responses_api_enabled = false;
    /** Prompt-cache switch; OpenAI uses it as prompt_cache_key, Anthropic as cache_control. */
    bool anthropic_cache_enabled = true;
    /** Anthropic-only cache TTL. */
    std::string anthropic_cache_ttl = "1h";
};

/** CustomProviderConfig.providerId: a stable id wins, otherwise the display name. */
std::string provider_id(const CustomProviderConfig& provider);

/** CustomProviderConfig.ownsIdentity: providerId match or a legacy display-name alias. */
bool owns_identity(const CustomProviderConfig& provider, std::string_view reference);

/**
 * Port of data/ModelThinkingCapabilityResolver.kt.
 *
 * The first matching custom provider in source order wins. Its protocol, not its display name,
 * selects the wire family; a provider with no match leaves the protocol unset.
 */
thinking_capability::Capability thinking_capability_for_selected_model(
    std::string_view selected_model, const std::vector<CustomProviderConfig>& custom_providers);

}  // namespace agora::core::model_thinking_resolver
