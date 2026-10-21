#pragma once

#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"

namespace agora::core::export_extra_settings {

/**
 * Port of data/ExportExtraSettings.kt::restoreLegacyFromJsonObject.
 *
 * This is the legacy JSON-object reader used when a backup manifest is older than version 4. It
 * does not touch system prompts (those are restored from system_prompts.json) and it intentionally
 * keeps kotlinx's primitive coercions: Int/Float are parsed from the primitive content, Boolean is
 * case-insensitive "true"/"false", and contentOrNull drops JsonNull while stringifying numbers and
 * booleans.
 */

/** data/SettingsContracts.kt::ConversationSettings. Only the fields the legacy reader writes. */
struct ConversationSettings {
    std::optional<int> context_window;
    std::optional<float> temperature;
    std::optional<int> max_tokens;
    std::optional<float> top_p;
    std::optional<float> frequency_penalty;
    std::optional<float> presence_penalty;
    std::optional<bool> code_execution_enabled;
    std::optional<bool> google_search_enabled;
    std::optional<bool> open_ai_web_search_enabled;
    std::optional<bool> thinking_enabled;
    std::optional<std::string> thinking_level;
    std::optional<bool> thinking_budget_enabled;
    std::optional<int> thinking_budget_tokens;
    std::optional<bool> open_ai_service_tier_enabled;
    std::optional<std::string> open_ai_service_tier;
    std::optional<bool> web_search_enabled;
    std::optional<bool> shell_enabled;
    std::optional<bool> low_context_mode_enabled;

    bool is_all_null() const;
};

using StringMap = std::vector<std::pair<std::string, std::string>>;

/** Everything the legacy reader does to SettingsManager, in the exact argument shape. */
class SettingsStore {
public:
    virtual ~SettingsStore() = default;

    virtual const StringMap& model_aliases() const = 0;
    virtual const std::vector<std::string>& custom_models() const = 0;

    virtual void save_image_transcription_enabled(bool enabled) = 0;
    virtual void save_image_transcription_enabled_models(
        const std::vector<std::string>& models) = 0;
    virtual void save_image_transcription_model(const std::string& model) = 0;
    virtual void save_image_transcription_batch_size(int size) = 0;
    virtual void save_image_transcription_prompt(const std::string& prompt) = 0;
    virtual void save_web_search_num_results(int count) = 0;
    virtual void save_search_context_window(int window) = 0;
    virtual void save_search_match_limit(int limit) = 0;
    virtual void save_default_temperature(float value) = 0;
    virtual void save_default_max_tokens(int value) = 0;
    virtual void save_default_top_p(float value) = 0;
    virtual void save_default_frequency_penalty(float value) = 0;
    virtual void save_default_presence_penalty(float value) = 0;
    virtual void save_conversation_settings(const std::string& conversation_id,
                                            const ConversationSettings& settings) = 0;
    virtual void save_show_documentation_fab(bool enabled) = 0;
    virtual void save_proxy_enabled(bool enabled) = 0;
    virtual void save_proxy_type(const std::string& type) = 0;
    virtual void save_proxy_host(const std::string& host) = 0;
    virtual void save_proxy_port(const std::string& port) = 0;
    virtual void save_proxy_username(const std::string& username) = 0;
    virtual void save_proxy_password(const std::string& password) = 0;
    virtual void save_proxy_bypass(const std::string& bypass) = 0;
    virtual void save_theme_mode(const std::string& mode) = 0;
    virtual void save_color_scheme(const std::string& scheme) = 0;
    virtual void save_dynamic_color(bool enabled) = 0;
    virtual void save_blur_effects_enabled(bool enabled) = 0;
    virtual void save_reduce_motion(bool enabled) = 0;
    virtual void save_open_ai_service_tier_enabled(bool enabled) = 0;
    virtual void save_open_ai_service_tier(const std::string& tier) = 0;
    virtual void save_haptics_enabled(bool enabled) = 0;
    virtual void save_detailed_token_usage(bool enabled) = 0;
    virtual void save_auto_expand_active_group(bool enabled) = 0;
    virtual void save_scheme_style(const std::string& style) = 0;
    virtual void save_font_preference(const std::string& preference) = 0;
    virtual void save_auto_update_check(bool enabled) = 0;
    virtual void save_automation_tools_enabled(bool enabled) = 0;
    virtual void save_exact_execution_enabled(bool enabled) = 0;
    virtual void save_automation_wake_lock_enabled(bool enabled) = 0;
    virtual void save_model_aliases(const StringMap& aliases) = 0;
    virtual void save_custom_models(const std::vector<std::string>& models) = 0;
};

struct RestoreOptions {
    bool replace = false;
    bool allow_secrets = false;
    std::vector<std::string> allowed_conversation_ids;
};

/**
 * Kotlin exception categories. Messages are kotlinx/JVM diagnostics and are not part of the
 * settings contract; callers can distinguish the categories by dynamic type.
 */
class KotlinException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    virtual const char* kotlin_type() const noexcept = 0;
};

/** java.lang.IllegalArgumentException: a JsonElement has the wrong shape. */
class IllegalArgumentException : public KotlinException {
public:
    using KotlinException::KotlinException;
    const char* kotlin_type() const noexcept override { return "IllegalArgumentException"; }
};

/** java.lang.NumberFormatException (an IllegalArgumentException): Int/Float content is invalid. */
class NumberFormatException : public IllegalArgumentException {
public:
    using IllegalArgumentException::IllegalArgumentException;
    const char* kotlin_type() const noexcept override { return "NumberFormatException"; }
};

/** java.lang.IllegalStateException: a primitive does not represent a Boolean. */
class IllegalStateException : public KotlinException {
public:
    using KotlinException::KotlinException;
    const char* kotlin_type() const noexcept override { return "IllegalStateException"; }
};

/**
 * Applies the legacy settings object exactly like the Kotlin reader. `obj` must be a JsonObject.
 * Throws the KotlinException subclasses above when a present field has an invalid shape or content;
 * writes performed before the throw stay applied, matching the sequential Kotlin code.
 */
void restore_legacy_from_json_object(const json::Value& obj, SettingsStore& settings,
                                     const RestoreOptions& options);

}  // namespace agora::core::export_extra_settings
