// Port of data/ExportExtraSettings.kt::restoreLegacyFromJsonObject.
//
// The Kotlin reader is a sequence of nullable JSON lookups; a present but mistyped value throws
// before the save it guards, and earlier saves stay applied. The order below is the source order.
#include "agora/core/export_extra_settings.hpp"

#include <cctype>
#include <cerrno>
#include <cmath>
#include <cstdlib>
#include <limits>
#include <string>
#include <unordered_set>

#include "agora/core/openai_service_tiers.hpp"
#include "agora/core/thinking_capability.hpp"

namespace agora::core::export_extra_settings {
namespace {

using json::Kind;
using json::Value;

const Value* field(const Value& object, std::string_view name) {
    const Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;  // last duplicate wins
    }
    return found;
}

/** JsonElement.jsonPrimitive: arrays and objects throw IllegalArgumentException. */
const Value& require_primitive(const Value& value) {
    if (value.kind == Kind::Array || value.kind == Kind::Object) {
        throw IllegalArgumentException("element is not a JsonPrimitive");
    }
    return value;
}

bool is_null_literal(const Value& value) {
    return value.kind == Kind::Literal && value.text == "null";
}

std::string ascii_lower(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

/** JsonPrimitive.contentOrNull, after the JsonElement.jsonPrimitive cast. */
std::optional<std::string> primitive_content(const Value* value) {
    if (value == nullptr) return std::nullopt;
    const Value& primitive = require_primitive(*value);
    if (is_null_literal(primitive)) return std::nullopt;
    return primitive.text;
}

/**
 * kotlinx JsonPrimitive.int as observed for this reader. The content is trimmed and read as a
 * decimal literal: optional '-', ASCII digits (leading zeros allowed), and an optional decimal
 * exponent. A negative exponent is accepted only when it does not leave a fractional part; the
 * result must fit in Int. '+' and '.' are rejected, matching the reference traces.
 */
int parse_int_content(std::string_view raw) {
    // The reference parser trims the same bytes as java.lang.String.trim() (<= U+0020), not the
    // wider Kotlin whitespace set: NBSP and em space stay in the content and fail the parse.
    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && static_cast<unsigned char>(raw[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(raw[end - 1]) <= 0x20) --end;
    const std::string text(raw.substr(begin, end - begin));
    const auto fail = []() -> int {
        throw NumberFormatException("content does not represent an Int");
    };
    if (text.empty()) return fail();

    std::size_t pos = 0;
    bool negative = false;
    if (text[pos] == '-') {
        negative = true;
        ++pos;
    }
    const std::size_t digits_begin = pos;
    while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') ++pos;
    if (pos == digits_begin) return fail();

    long long exponent = 0;
    if (pos < text.size() && (text[pos] == 'e' || text[pos] == 'E')) {
        ++pos;
        bool exponent_negative = false;
        if (pos < text.size() && (text[pos] == '+' || text[pos] == '-')) {
            exponent_negative = text[pos] == '-';
            ++pos;
        }
        const std::size_t exponent_begin = pos;
        while (pos < text.size() && text[pos] >= '0' && text[pos] <= '9') {
            if (exponent < 100000) exponent = exponent * 10 + (text[pos] - '0');
            ++pos;
        }
        if (pos != text.size()) return fail();
        if (pos == exponent_begin) exponent = 0;  // "3e" is accepted by the reference parser
        if (exponent_negative) exponent = -exponent;
    }
    if (pos != text.size()) return fail();

    const std::size_t mantissa_end = text.find_first_of("eE", digits_begin);
    std::string digits = text.substr(digits_begin, mantissa_end == std::string::npos
                                                      ? std::string::npos
                                                      : mantissa_end - digits_begin);

    std::size_t first_nonzero = digits.find_first_not_of('0');
    if (first_nonzero == std::string::npos) return 0;
    digits.erase(0, first_nonzero);

    if (exponent < 0) {
        const auto drop = static_cast<std::size_t>(-exponent);
        if (digits.size() <= drop) return fail();  // a fractional remainder would remain
        for (std::size_t i = digits.size() - drop; i < digits.size(); ++i) {
            if (digits[i] != '0') return fail();
        }
        digits.resize(digits.size() - drop);
    } else {
        if (exponent > 20) return fail();
        digits.append(static_cast<std::size_t>(exponent), '0');
    }

    if (digits.size() > 10) return fail();
    long long magnitude = 0;
    for (char digit : digits) magnitude = magnitude * 10 + (digit - '0');
    if (negative) magnitude = -magnitude;
    if (magnitude < std::numeric_limits<int>::min() ||
        magnitude > std::numeric_limits<int>::max()) {
        return fail();
    }
    return static_cast<int>(magnitude);
}

/**
 * java.lang.Float.parseFloat for the primitive content. Only the exact "NaN"/"Infinity" spellings
 * are special (lowercase inf/nan are rejected, unlike C strtof); decimal and hexadecimal forms,
 * a trailing f/F/d/D suffix, and leading/trailing whitespace follow the JVM parser. Overflow to
 * infinity and underflow to zero are Java behaviour and are kept.
 */
float parse_float_content(std::string_view raw) {
    const auto fail = []() -> float {
        throw NumberFormatException("content does not represent a Float");
    };

    std::size_t begin = 0;
    std::size_t end = raw.size();
    while (begin < end && static_cast<unsigned char>(raw[begin]) <= 0x20) ++begin;
    while (end > begin && static_cast<unsigned char>(raw[end - 1]) <= 0x20) --end;
    std::string text(raw.substr(begin, end - begin));
    if (text.empty()) return fail();

    std::size_t sign = 0;
    if (text[0] == '+' || text[0] == '-') sign = 1;
    const std::string_view body(text.data() + sign, text.size() - sign);
    if (body == "NaN" || body == "Infinity") {
        if (body == "NaN") return std::numeric_limits<float>::quiet_NaN();
        return text[0] == '-' ? -std::numeric_limits<float>::infinity()
                              : std::numeric_limits<float>::infinity();
    }

    // A trailing Java suffix is part of the grammar; strip exactly one before strtof.
    if (text.size() > 1) {
        const char last = text.back();
        if (last == 'f' || last == 'F' || last == 'd' || last == 'D') text.pop_back();
    }
    if (text.empty()) return fail();

    // Reject alphabetic spellings that strtof would otherwise accept case-insensitively. Java's
    // hexadecimal FloatValue also requires a binary exponent ("0x1.8p1"), unlike strtof.
    const std::size_t start = (text[0] == '+' || text[0] == '-') ? 1 : 0;
    const bool hex = text.compare(start, 2, "0x") == 0 || text.compare(start, 2, "0X") == 0;
    if (hex && text.find_first_of("pP", start + 2) == std::string::npos) return fail();
    if (start < text.size() && std::isalpha(static_cast<unsigned char>(text[start])) != 0) {
        if (!hex) return fail();
    }

    errno = 0;
    char* parse_end = nullptr;
    const float value = std::strtof(text.c_str(), &parse_end);
    if (parse_end != text.c_str() + text.size()) return fail();
    return value;
}

std::optional<bool> boolean_value(const Value* value) {
    if (value == nullptr) return std::nullopt;
    const Value& primitive = require_primitive(*value);
    const std::string content = ascii_lower(primitive.text);
    if (content == "true") return true;
    if (content == "false") return false;
    throw IllegalStateException("content does not represent a Boolean");
}

std::optional<int> int_value(const Value* value) {
    if (value == nullptr) return std::nullopt;
    const Value& primitive = require_primitive(*value);
    return parse_int_content(primitive.text);
}

std::optional<float> float_value(const Value* value) {
    if (value == nullptr) return std::nullopt;
    const Value& primitive = require_primitive(*value);
    return parse_float_content(primitive.text);
}

/** JsonElement.jsonObject: anything else throws IllegalArgumentException. */
const Value& require_object(const Value& value) {
    if (value.kind != Kind::Object) {
        throw IllegalArgumentException("element is not a JsonObject");
    }
    return value;
}

/** JsonObject iteration order: first insertion position, last value for duplicate keys. */
std::vector<std::pair<std::string, const Value*>> object_entries(const Value& object) {
    std::vector<std::pair<std::string, const Value*>> entries;
    for (const json::Member& member : object.members) {
        bool replaced = false;
        for (auto& entry : entries) {
            if (entry.first == member.key) {
                entry.second = &member.value;
                replaced = true;
                break;
            }
        }
        if (!replaced) entries.emplace_back(member.key, &member.value);
    }
    return entries;
}

bool contains(const std::vector<std::string>& values, std::string_view wanted) {
    for (const std::string& value : values) {
        if (value == wanted) return true;
    }
    return false;
}

void put_alias(StringMap& map, const std::string& key, std::string value) {
    for (auto& entry : map) {
        if (entry.first == key) {
            entry.second = std::move(value);
            return;
        }
    }
    map.emplace_back(key, std::move(value));
}

void put_model(std::vector<std::string>& models, const std::string& model) {
    if (contains(models, model)) return;
    models.push_back(model);
}

ConversationSettings read_conversation(const Value& value) {
    const Value& object = require_object(value);

    const std::optional<std::string> raw_level = primitive_content(field(object, "thinkingLevel"));
    const std::optional<int> legacy_budget =
        thinking_capability::legacy_budget_tokens(raw_level);

    ConversationSettings settings;
    settings.context_window = int_value(field(object, "contextWindow"));
    settings.temperature = float_value(field(object, "temperature"));
    settings.max_tokens = int_value(field(object, "maxTokens"));
    settings.top_p = float_value(field(object, "topP"));
    settings.frequency_penalty = float_value(field(object, "frequencyPenalty"));
    settings.presence_penalty = float_value(field(object, "presencePenalty"));
    settings.code_execution_enabled = boolean_value(field(object, "codeExecutionEnabled"));
    settings.google_search_enabled = boolean_value(field(object, "googleSearchEnabled"));
    settings.open_ai_web_search_enabled =
        boolean_value(field(object, "openAiWebSearchEnabled"));
    settings.thinking_enabled = boolean_value(field(object, "thinkingEnabled"));

    const std::optional<std::string> level = primitive_content(field(object, "thinkingLevel"));
    if (level.has_value()) settings.thinking_level = thinking_capability::normalize(level);

    settings.thinking_budget_enabled = boolean_value(field(object, "thinkingBudgetEnabled"));
    if (!settings.thinking_budget_enabled.has_value() && legacy_budget.has_value()) {
        settings.thinking_budget_enabled = true;
    }
    settings.thinking_budget_tokens = int_value(field(object, "thinkingBudgetTokens"));
    if (!settings.thinking_budget_tokens.has_value()) {
        settings.thinking_budget_tokens = legacy_budget;
    }

    settings.open_ai_service_tier_enabled =
        boolean_value(field(object, "openAiServiceTierEnabled"));
    const std::optional<std::string> tier = primitive_content(field(object, "openAiServiceTier"));
    if (tier.has_value()) settings.open_ai_service_tier = openai_service_tiers::normalize(tier);

    settings.web_search_enabled = boolean_value(field(object, "webSearchEnabled"));
    settings.shell_enabled = boolean_value(field(object, "shellEnabled"));
    settings.low_context_mode_enabled = boolean_value(field(object, "lowContextModeEnabled"));
    return settings;
}

}  // namespace

bool ConversationSettings::is_all_null() const {
    return !context_window.has_value() && !temperature.has_value() && !max_tokens.has_value() &&
           !top_p.has_value() && !frequency_penalty.has_value() &&
           !presence_penalty.has_value() && !code_execution_enabled.has_value() &&
           !google_search_enabled.has_value() && !open_ai_web_search_enabled.has_value() &&
           !thinking_enabled.has_value() && !thinking_level.has_value() &&
           !thinking_budget_enabled.has_value() && !thinking_budget_tokens.has_value() &&
           !open_ai_service_tier_enabled.has_value() && !open_ai_service_tier.has_value() &&
           !web_search_enabled.has_value() && !shell_enabled.has_value() &&
           !low_context_mode_enabled.has_value();
}

void restore_legacy_from_json_object(const Value& obj, SettingsStore& settings,
                                     const RestoreOptions& options) {
    require_object(obj);

    // System prompts are restored standalone from system_prompts.json, not here.
    if (const std::optional<bool> value = boolean_value(field(obj, "imageTranscriptionEnabled"))) {
        settings.save_image_transcription_enabled(*value);
    }
    if (const std::optional<std::string> value =
            primitive_content(field(obj, "imageTranscriptionEnabledModels"))) {
        std::vector<std::string> models;
        std::size_t begin = 0;
        while (begin <= value->size()) {
            const std::size_t comma = value->find(',', begin);
            const std::size_t end = comma == std::string::npos ? value->size() : comma;
            const std::string_view part(value->data() + begin, end - begin);
            if (!json::is_blank(part) && !contains(models, part)) {
                models.emplace_back(part);
            }
            if (comma == std::string::npos) break;
            begin = comma + 1;
        }
        settings.save_image_transcription_enabled_models(models);
    }
    if (const std::optional<std::string> value =
            primitive_content(field(obj, "imageTranscriptionModel"))) {
        settings.save_image_transcription_model(*value);
    }
    if (const std::optional<int> value = int_value(field(obj, "imageTranscriptionBatchSize"))) {
        settings.save_image_transcription_batch_size(*value);
    }
    if (const std::optional<std::string> value =
            primitive_content(field(obj, "imageTranscriptionPrompt"))) {
        settings.save_image_transcription_prompt(*value);
    }
    if (const std::optional<int> value = int_value(field(obj, "webSearchNumResults"))) {
        settings.save_web_search_num_results(*value);
    }
    if (const std::optional<int> value = int_value(field(obj, "searchContextWindow"))) {
        settings.save_search_context_window(*value);
    }
    if (const std::optional<int> value = int_value(field(obj, "searchMatchLimit"))) {
        settings.save_search_match_limit(*value);
    }
    if (const std::optional<float> value = float_value(field(obj, "defaultTemperature"))) {
        settings.save_default_temperature(*value);
    }
    if (const std::optional<int> value = int_value(field(obj, "defaultMaxTokens"))) {
        settings.save_default_max_tokens(*value);
    }
    if (const std::optional<float> value = float_value(field(obj, "defaultTopP"))) {
        settings.save_default_top_p(*value);
    }
    if (const std::optional<float> value = float_value(field(obj, "defaultFrequencyPenalty"))) {
        settings.save_default_frequency_penalty(*value);
    }
    if (const std::optional<float> value = float_value(field(obj, "defaultPresencePenalty"))) {
        settings.save_default_presence_penalty(*value);
    }

    if (const Value* conversations_value = field(obj, "conversationSettings")) {
        const Value& conversations = require_object(*conversations_value);
        for (const auto& [conversation_id, settings_value] : object_entries(conversations)) {
            if (!contains(options.allowed_conversation_ids, conversation_id)) continue;
            const ConversationSettings conversation = read_conversation(*settings_value);
            if (!conversation.is_all_null()) {
                settings.save_conversation_settings(conversation_id, conversation);
            }
        }
    }

    if (const std::optional<bool> value = boolean_value(field(obj, "showDocumentationFab"))) {
        settings.save_show_documentation_fab(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "proxyEnabled"))) {
        settings.save_proxy_enabled(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "proxyType"))) {
        settings.save_proxy_type(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "proxyHost"))) {
        settings.save_proxy_host(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "proxyPort"))) {
        settings.save_proxy_port(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "proxyUsername"))) {
        settings.save_proxy_username(*value);
    }
    if (options.allow_secrets) {
        if (const std::optional<std::string> value =
                primitive_content(field(obj, "proxyPassword"))) {
            if (!value->empty()) settings.save_proxy_password(*value);
        }
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "proxyBypass"))) {
        if (!value->empty()) settings.save_proxy_bypass(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "themeMode"))) {
        settings.save_theme_mode(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "colorScheme"))) {
        settings.save_color_scheme(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "dynamicColor"))) {
        settings.save_dynamic_color(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "blurEffectsEnabled"))) {
        settings.save_blur_effects_enabled(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "reduceMotion"))) {
        settings.save_reduce_motion(*value);
    }
    if (const std::optional<bool> value =
            boolean_value(field(obj, "openAiServiceTierEnabled"))) {
        settings.save_open_ai_service_tier_enabled(*value);
    }
    if (const std::optional<std::string> value =
            primitive_content(field(obj, "openAiServiceTier"))) {
        settings.save_open_ai_service_tier(openai_service_tiers::normalize(value));
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "hapticsEnabled"))) {
        settings.save_haptics_enabled(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "detailedTokenUsage"))) {
        settings.save_detailed_token_usage(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "autoExpandActiveGroup"))) {
        settings.save_auto_expand_active_group(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "schemeStyle"))) {
        settings.save_scheme_style(*value);
    }
    if (const std::optional<std::string> value = primitive_content(field(obj, "fontPreference"))) {
        settings.save_font_preference(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "autoUpdateCheck"))) {
        settings.save_auto_update_check(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "automationToolsEnabled"))) {
        settings.save_automation_tools_enabled(*value);
    }
    if (const std::optional<bool> value = boolean_value(field(obj, "exactExecutionEnabled"))) {
        settings.save_exact_execution_enabled(*value);
    }
    if (const std::optional<bool> value =
            boolean_value(field(obj, "automationWakeLockEnabled"))) {
        settings.save_automation_wake_lock_enabled(*value);
    }

    if (const Value* aliases_value = field(obj, "modelAliases")) {
        const Value& aliases_object = require_object(*aliases_value);
        StringMap imported;
        for (const auto& [key, value] : object_entries(aliases_object)) {
            const std::optional<std::string> content = primitive_content(value);
            if (content.has_value()) put_alias(imported, key, *content);
        }
        if (!imported.empty()) {
            if (options.replace) {
                settings.save_model_aliases(imported);
            } else {
                StringMap merged = settings.model_aliases();
                for (const auto& [key, value] : imported) put_alias(merged, key, value);
                settings.save_model_aliases(merged);
            }
        }
    }

    if (const Value* models_value = field(obj, "customModels")) {
        if (models_value->kind == Kind::Array) {
            std::vector<std::string> imported;
            for (const Value& element : models_value->elements) {
                const std::optional<std::string> content = primitive_content(&element);
                if (content.has_value()) put_model(imported, *content);
            }
            if (options.replace) {
                settings.save_custom_models(imported);
            } else {
                std::vector<std::string> merged = settings.custom_models();
                for (const std::string& model : imported) put_model(merged, model);
                settings.save_custom_models(merged);
            }
        }
    }
}

}  // namespace agora::core::export_extra_settings
