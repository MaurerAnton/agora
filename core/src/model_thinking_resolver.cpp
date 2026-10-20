#include "agora/core/model_thinking_resolver.hpp"

#include <algorithm>
#include <cctype>
#include <optional>
#include <string>
#include <string_view>
#include <utility>

#include "agora/core/json.hpp"
#include "agora/core/provider_identity.hpp"

namespace agora::core::model_thinking_resolver {
namespace {

bool contains(std::string_view haystack, std::string_view needle) {
    return haystack.find(needle) != std::string_view::npos;
}

/** Kotlin String.lowercase() for the ASCII protocol spellings this enum accepts. */
std::string ascii_lower(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (unsigned char c : text) out.push_back(static_cast<char>(std::tolower(c)));
    return out;
}

}  // namespace

std::string ModelId::api_model_name() const {
    constexpr std::string_view kModelsPrefix = "models/";
    if (model_name.rfind(kModelsPrefix, 0) == 0) {
        return model_name.substr(kModelsPrefix.size());
    }
    return model_name;
}

ModelId parse_model_id(std::string_view prefixed) {
    const std::size_t colon = prefixed.find(':');
    if (colon != std::string_view::npos) {
        return ModelId{std::string(prefixed.substr(0, colon)),
                       std::string(prefixed.substr(colon + 1))};
    }

    // Legacy / unprefixed model IDs. This is the single source of truth shared with
    // ProviderRegistry.providerForModel; keep the order and case sensitivity of the Kotlin when.
    std::string provider;
    if (prefixed.rfind("gpt-", 0) == 0 || prefixed.rfind("o1", 0) == 0 ||
        prefixed.rfind("o3", 0) == 0 || prefixed.rfind("o4", 0) == 0) {
        provider = "OpenAI";
    } else if (prefixed.rfind("claude-", 0) == 0) {
        provider = "Anthropic";
    } else if (contains(prefixed, "deepseek")) {
        provider = "DeepSeek";
    } else if (contains(prefixed, "qwen")) {
        provider = "Qwen";
    } else if (contains(prefixed, "groq")) {
        provider = "Groq";
    } else if (contains(prefixed, "models/") || prefixed.rfind("gemini", 0) == 0) {
        provider = "Google";
    } else {
        provider = "Unknown";
    }
    return ModelId{std::move(provider), std::string(prefixed)};
}

std::string_view wire_value(CustomEndpointProtocol protocol) {
    switch (protocol) {
        case CustomEndpointProtocol::openai: return "openai";
        case CustomEndpointProtocol::google: return "google";
        case CustomEndpointProtocol::anthropic: return "anthropic";
        case CustomEndpointProtocol::unknown: return "unknown";
    }
    return "unknown";
}

CustomEndpointProtocol protocol_from_wire_value(std::string_view value) {
    // Kotlin: value.trim().lowercase(); the accepted spellings are ASCII.
    const std::string normalized = ascii_lower(json::trim(value));
    if (normalized == "openai") return CustomEndpointProtocol::openai;
    if (normalized == "google" || normalized == "gemini") return CustomEndpointProtocol::google;
    if (normalized == "anthropic" || normalized == "claude") {
        return CustomEndpointProtocol::anthropic;
    }
    return CustomEndpointProtocol::unknown;
}

std::string provider_id(const CustomProviderConfig& provider) {
    // takeIf(isStableId) returns the original string; isStableId itself trims only for the test.
    if (provider_identity::is_stable_id(provider.id)) return provider.id;
    return provider.name;
}

bool owns_identity(const CustomProviderConfig& provider, std::string_view reference) {
    if (provider_id(provider) == reference) return true;
    return std::find(provider.legacy_names.begin(), provider.legacy_names.end(), reference) !=
           provider.legacy_names.end();
}

thinking_capability::Capability thinking_capability_for_selected_model(
    std::string_view selected_model, const std::vector<CustomProviderConfig>& custom_providers) {
    const ModelId parsed = parse_model_id(selected_model);
    const CustomProviderConfig* custom = nullptr;
    for (const CustomProviderConfig& candidate : custom_providers) {
        if (candidate.name == parsed.provider_name ||
            owns_identity(candidate, parsed.provider_name)) {
            custom = &candidate;
            break;
        }
    }
    const std::optional<std::string> protocol =
        custom == nullptr
            ? std::nullopt
            : std::optional<std::string>(std::string(wire_value(custom->protocol)));
    return thinking_capability::for_provider(parsed.provider_name, parsed.model_name, protocol);
}

}  // namespace agora::core::model_thinking_resolver
