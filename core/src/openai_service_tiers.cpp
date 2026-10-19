// Port of model/OpenAiServiceTiers.kt.
//
// The tables are the app's model lists verbatim; the mapping rules are pure set lookups. Kotlin's
// `lowercase()` is Unicode-aware while this port folds ASCII only — every tier string and every
// model id is ASCII, and a non-ASCII lookalike could only ever *fail* to match a table entry in
// both implementations except for exotic Unicode folds such as the Kelvin sign. The scenarios pin
// the ASCII behaviour.
#include "agora/core/openai_service_tiers.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_set>

#include "agora/core/json.hpp"

namespace agora::core::openai_service_tiers {
namespace {

constexpr std::string_view kAuto = "auto";
constexpr std::string_view kDefault = "default";
constexpr std::string_view kFlex = "flex";
constexpr std::string_view kFast = "fast";
constexpr std::string_view kUltrafast = "ultrafast";

constexpr std::string_view kUltrafastModel = "gpt-5.6-sol";

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

const std::unordered_set<std::string>& flex_models() {
    static const std::unordered_set<std::string> models = {
        "gpt-6-astra", "gpt-6-sol", "gpt-6-luna",
        "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
        "gpt-5.5", "gpt-5.5-pro",
        "gpt-5.4", "gpt-5.4-mini", "gpt-5.4-nano", "gpt-5.4-pro",
        "gpt-5.2", "gpt-5.1", "gpt-5", "gpt-5-mini", "gpt-5-nano",
        "o3", "o4-mini",
    };
    return models;
}

const std::unordered_set<std::string>& fast_models() {
    static const std::unordered_set<std::string> models = {
        "gpt-6-astra", "gpt-6-sol", "gpt-6-luna",
        "gpt-5.6-sol", "gpt-5.6-terra", "gpt-5.6-luna",
        "gpt-5.5", "gpt-5.4", "gpt-5.4-mini",
        "gpt-5.2", "gpt-5.1", "gpt-5", "gpt-5-mini",
        "gpt-4.1", "gpt-4.1-mini", "gpt-4.1-nano",
        "gpt-4o", "gpt-4o-2024-05-13", "gpt-4o-mini",
        "o3", "o4-mini", "gpt-5.3-codex",
    };
    return models;
}

const std::unordered_set<std::string>& standard_only_models() {
    static const std::unordered_set<std::string> models = {
        "gpt-5.2-pro", "gpt-5-pro", "o1", "o1-pro", "o3-pro", "o3-mini",
        "gpt-4-turbo-2024-04-09", "gpt-4-0613", "gpt-3.5-turbo",
        "gpt-3.5-turbo-0125", "gpt-3.5-turbo-1106", "gpt-3.5-turbo-instruct",
        "davinci-002", "babbage-002",
    };
    return models;
}

bool is_known_model(const std::string& model) {
    return flex_models().count(model) != 0 || fast_models().count(model) != 0 ||
           standard_only_models().count(model) != 0;
}

/** Kotlin `modelId?.trim()?.lowercase()`; nullopt when the model id is absent. */
std::optional<std::string> normalize_model(const std::optional<std::string>& model_id) {
    if (!model_id.has_value()) return std::nullopt;
    return ascii_lower(json::trim(*model_id));
}

}  // namespace

const std::vector<std::string>& values() {
    static const std::vector<std::string> kValues = {"auto", "default", "flex", "fast", "ultrafast"};
    return kValues;
}

std::string normalize(const std::optional<std::string>& value) {
    std::string normalized = ascii_lower(json::trim(value.value_or(std::string())));
    if (normalized == "scale") return std::string(kDefault);
    if (normalized == "priority") return std::string(kFast);
    for (const std::string& candidate : values()) {
        if (candidate == normalized) return normalized;
    }
    return std::string(kAuto);
}

std::vector<std::string> available_tiers(const std::optional<std::string>& model_id,
                                         bool official_provider) {
    if (!official_provider) return values();

    const std::optional<std::string> normalized = normalize_model(model_id);
    if (!normalized.has_value() || !is_known_model(*normalized)) return values();
    const std::string& model = *normalized;

    std::vector<std::string> supported;
    for (const std::string& tier : values()) {
        bool keep = true;
        if (tier == kFlex) {
            keep = flex_models().count(model) != 0;
        } else if (tier == kFast) {
            keep = fast_models().count(model) != 0;
        } else if (tier == kUltrafast) {
            keep = model == kUltrafastModel;
        }
        if (keep) supported.push_back(tier);
    }
    return supported;
}

std::string mapped_tier(const std::optional<std::string>& value,
                        const std::optional<std::string>& model_id,
                        bool official_provider) {
    const std::string tier = normalize(value);
    const std::vector<std::string> supported = available_tiers(model_id, official_provider);
    if (std::find(supported.begin(), supported.end(), tier) != supported.end()) return tier;
    if (tier == kFlex) return std::string(kDefault);
    const auto supports = [&supported](std::string_view candidate) {
        return std::find(supported.begin(), supported.end(), candidate) != supported.end();
    };
    if (tier == kUltrafast) return supports(kFast) ? std::string(kFast) : std::string(kDefault);
    if (tier == kFast) return supports(kUltrafast) ? std::string(kUltrafast) : std::string(kDefault);
    return std::string(kDefault);
}

int index_for_tier(const std::optional<std::string>& value) {
    const std::string normalized = normalize(value);
    const std::vector<std::string>& all = values();
    for (std::size_t index = 0; index < all.size(); ++index) {
        if (all[index] == normalized) return static_cast<int>(index);
    }
    return 0;  // unreachable: normalize always returns one of values()
}

std::string tier_for_index(int index) {
    const std::vector<std::string>& all = values();
    const int last = static_cast<int>(all.size()) - 1;
    if (index < 0) index = 0;
    if (index > last) index = last;
    return all[static_cast<std::size_t>(index)];
}

std::optional<std::string> request_value(bool enabled,
                                         const std::optional<std::string>& value,
                                         bool responses_api_enabled,
                                         const std::optional<std::string>& model_id,
                                         bool official_provider) {
    if (!enabled || !responses_api_enabled) return std::nullopt;
    return mapped_tier(value, model_id, official_provider);
}

}  // namespace agora::core::openai_service_tiers
