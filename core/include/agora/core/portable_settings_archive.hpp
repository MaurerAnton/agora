#pragma once

#include <functional>
#include <optional>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/native_backup_secrets.hpp"
#include "agora/core/provider_identity.hpp"

namespace agora::core::settings_archive {

/**
 * Port of data/PortableSettingsArchive.kt: the portable settings allowlist that native backup
 * v4+ writes into settings.json (toJsonObject) and reads back on import
 * (restoreFromJsonObject, including prepareImportedCustomProviders).
 *
 * Boundary: SettingsManager is the Android DataStore storage class. The port splits it into
 *   SettingsSnapshot — the 97 flows the archive reads (the caller seeds them from the real
 *                     store; the golden double refuses a snapshot with a missing key);
 *   SettingsSink     — the save methods the archive calls, in call order, with the exact
 *                     arguments the archive passes. Storage-side coercions (clamping, blank
 *                     filtering, re-normalisation) belong to SettingsManager and are not
 *                     reproduced here.
 * The two injected callbacks of the real call site (resolveSystemPromptId = promptImport::resolve,
 * and the provider-id entropy in CustomProviderIdentityPolicy.newId) are parameters, never hidden.
 */

using native_backup::EmbeddingModelConfig;
using native_backup::EmbeddingModelType;
using native_backup::McpServerConfig;
using native_backup::ShellDeviceConfig;
using provider_identity::ReferenceMap;

/** data/CustomProviderConfig.kt::CustomProviderConfig (all fields; protocol is the wire value). */
struct CustomProviderConfig {
    std::string name;
    std::string protocol = "openai";
    std::string id;
    std::vector<std::string> legacy_names;
    bool responses_api_enabled = false;
    bool anthropic_cache_enabled = true;
    std::string anthropic_cache_ttl = "1h";

    /** CustomProviderConfig.providerId: `id` when it is a stable id, otherwise `name`. */
    std::string provider_id() const;
};

using StringMap = std::vector<std::pair<std::string, std::string>>;
using BoolMap = std::vector<std::pair<std::string, bool>>;

/**
 * The 97 flows the archive reads, in source order. Sets and maps keep Kotlin's insertion order
 * (LinkedHashSet/LinkedHashMap), which is observable in the exported JSON and in merges.
 */
struct SettingsSnapshot {
    std::string selected_model;
    std::vector<std::string> custom_models;
    std::vector<std::string> enabled_models;
    StringMap model_aliases;
    BoolMap model_provider_names;
    int max_context_window = 0;
    bool visualize_context_rollout = false;
    bool context_compact_enabled = false;
    std::optional<std::string> context_compact_model;
    std::string context_compact_prompt;
    int context_compact_retain_count = 0;
    bool context_compact_preserve_system_prompt = false;
    int context_compact_threshold_percent = 0;
    bool code_execution_enabled = false;
    bool google_search_enabled = false;
    bool thinking_enabled = false;
    std::string thinking_level;
    bool thinking_budget_enabled = false;
    int thinking_budget_tokens = 0;
    bool open_ai_service_tier_enabled = false;
    std::string open_ai_service_tier;
    bool open_ai_responses_api_enabled = false;
    bool anthropic_cache_enabled = false;
    std::string anthropic_cache_ttl;
    StringMap provider_base_urls;
    bool title_generation_enabled = false;
    std::optional<std::string> title_generation_model;
    std::string title_generation_prompt;
    bool title_generation_notifications_enabled = false;
    bool access_past_conversations = false;
    bool access_saved_memories = false;
    bool access_active_memory = false;
    bool access_skills = false;
    bool rag_search_enabled = false;
    std::string model_search_method;
    std::string manual_search_method;
    std::vector<EmbeddingModelConfig> embedding_models;
    std::string active_embedding_model_id;
    std::string app_language;
    bool web_search_enabled = false;
    std::string web_search_provider;
    int web_search_num_results = 0;
    std::string web_search_base_url;
    bool image_gen_enabled = false;
    std::optional<std::string> image_gen_model;
    std::string image_gen_size;
    int search_context_window = 0;
    int search_match_limit = 0;
    float rag_threshold = 0.0f;
    bool auto_cache_enabled = false;
    bool show_uncached_notification = false;
    bool auto_update_check = false;
    bool image_transcription_enabled = false;
    std::vector<std::string> image_transcription_enabled_models;
    std::optional<std::string> image_transcription_model;
    int image_transcription_batch_size = 0;
    std::string image_transcription_prompt;
    bool shell_enabled = false;
    bool shell_confirm_enabled = false;
    bool ask_user_enabled = false;
    std::vector<ShellDeviceConfig> shell_devices;
    bool automation_tools_enabled = false;
    bool exact_execution_enabled = false;
    bool automation_wake_lock_enabled = false;
    std::vector<CustomProviderConfig> custom_providers;
    std::vector<McpServerConfig> mcp_servers;
    bool proxy_enabled = false;
    std::string proxy_type;
    std::string proxy_host;
    std::string proxy_port;
    std::string proxy_username;
    std::string proxy_bypass;
    bool show_documentation_fab = false;
    std::string theme_mode;
    bool amoled_enabled = false;
    std::string color_scheme;
    bool dynamic_color = false;
    bool blur_effects_enabled = false;
    bool reduce_motion = false;
    bool stick_to_bottom = false;
    bool parse_inline_dollar_math = false;
    bool auto_wrap_code_blocks = false;
    bool haptics_enabled = false;
    bool detailed_token_usage = false;
    std::string tool_call_display_mode;
    std::string thinking_segment_display_mode;
    bool auto_expand_active_group = false;
    std::string scheme_style;
    std::string font_preference;
    std::string custom_font_name;
    std::string custom_font_path;
    std::optional<float> default_temperature;
    std::optional<int> default_max_tokens;
    std::optional<float> default_top_p;
    std::optional<float> default_frequency_penalty;
    std::optional<float> default_presence_penalty;
    std::optional<std::string> active_system_prompt_id;
};

/** The SettingsManager write surface restoreFromJsonObject drives. */
class SettingsSink {
public:
    virtual ~SettingsSink() = default;

    virtual void save_selected_model(const std::string& model) = 0;
    virtual void save_custom_models(const std::vector<std::string>& models) = 0;
    virtual void save_enabled_models(const std::vector<std::string>& models) = 0;
    virtual void save_model_aliases(const StringMap& aliases) = 0;
    virtual void save_model_provider_names(const BoolMap& values, bool replace) = 0;
    virtual void save_max_context_window(int window) = 0;
    virtual void save_visualize_context_rollout(bool enabled) = 0;
    virtual void save_context_compact_enabled(bool enabled) = 0;
    virtual void save_context_compact_model(const std::optional<std::string>& model) = 0;
    virtual void save_context_compact_prompt(const std::string& prompt) = 0;
    virtual void save_context_compact_retain_count(int count) = 0;
    virtual void save_context_compact_preserve_system_prompt(bool enabled) = 0;
    virtual void save_context_compact_threshold_percent(int percent) = 0;
    virtual void save_code_execution_enabled(bool enabled) = 0;
    virtual void save_google_search_enabled(bool enabled) = 0;
    virtual void save_thinking_enabled(bool enabled) = 0;
    virtual void save_thinking_level(const std::string& level) = 0;
    virtual void save_thinking_budget_enabled(bool enabled) = 0;
    virtual void save_thinking_budget_tokens(int tokens) = 0;
    virtual void save_open_ai_service_tier_enabled(bool enabled) = 0;
    virtual void save_open_ai_service_tier(const std::string& tier) = 0;
    virtual void save_open_ai_responses_api_enabled(bool enabled) = 0;
    virtual void save_anthropic_cache_enabled(bool enabled) = 0;
    virtual void save_anthropic_cache_ttl(const std::string& ttl) = 0;
    virtual void save_provider_base_urls(const StringMap& urls) = 0;
    virtual void save_title_generation_enabled(bool enabled) = 0;
    virtual void save_title_generation_model(const std::optional<std::string>& model) = 0;
    virtual void save_title_generation_prompt(const std::string& prompt) = 0;
    virtual void save_title_generation_notifications_enabled(bool enabled) = 0;
    virtual void save_access_past_conversations(bool enabled) = 0;
    virtual void save_access_saved_memories(bool enabled) = 0;
    virtual void save_access_active_memory(bool enabled) = 0;
    virtual void save_access_skills(bool enabled) = 0;
    virtual void save_rag_search_enabled(bool enabled) = 0;
    virtual void save_model_search_method(const std::string& method) = 0;
    virtual void save_manual_search_method(const std::string& method) = 0;
    virtual void save_embedding_models(const std::vector<EmbeddingModelConfig>& models) = 0;
    virtual void set_active_embedding_model_id(const std::string& id) = 0;
    virtual void save_app_language(const std::string& language) = 0;
    virtual void save_web_search_enabled(bool enabled) = 0;
    virtual void save_web_search_provider(const std::string& provider) = 0;
    virtual void save_web_search_num_results(int count) = 0;
    virtual void save_web_search_base_url(const std::string& url) = 0;
    virtual void save_image_gen_enabled(bool enabled) = 0;
    virtual void save_image_gen_model(const std::optional<std::string>& model) = 0;
    virtual void save_image_gen_size(const std::string& size) = 0;
    virtual void save_search_context_window(int window) = 0;
    virtual void save_search_match_limit(int limit) = 0;
    virtual void save_rag_threshold(float threshold) = 0;
    virtual void save_auto_cache_enabled(bool enabled) = 0;
    virtual void save_show_uncached_notification(bool enabled) = 0;
    virtual void save_auto_update_check(bool enabled) = 0;
    virtual void save_image_transcription_enabled(bool enabled) = 0;
    virtual void save_image_transcription_enabled_models(
        const std::vector<std::string>& models) = 0;
    virtual void save_image_transcription_model(const std::optional<std::string>& model) = 0;
    virtual void save_image_transcription_batch_size(int size) = 0;
    virtual void save_image_transcription_prompt(const std::string& prompt) = 0;
    virtual void save_shell_enabled(bool enabled) = 0;
    virtual void save_shell_confirm_enabled(bool enabled) = 0;
    virtual void save_ask_user_enabled(bool enabled) = 0;
    virtual void save_shell_devices(const std::vector<ShellDeviceConfig>& devices) = 0;
    virtual void save_automation_tools_enabled(bool enabled) = 0;
    virtual void save_exact_execution_enabled(bool enabled) = 0;
    virtual void save_automation_wake_lock_enabled(bool enabled) = 0;
    virtual void save_custom_providers(const std::vector<CustomProviderConfig>& providers) = 0;
    virtual void save_mcp_servers(const std::vector<McpServerConfig>& servers) = 0;
    virtual void save_proxy_enabled(bool enabled) = 0;
    virtual void save_proxy_type(const std::string& type) = 0;
    virtual void save_proxy_host(const std::string& host) = 0;
    virtual void save_proxy_port(const std::string& port) = 0;
    virtual void save_proxy_username(const std::string& username) = 0;
    virtual void save_proxy_bypass(const std::string& bypass) = 0;
    virtual void save_show_documentation_fab(bool enabled) = 0;
    virtual void save_theme_mode(const std::string& mode) = 0;
    virtual void save_amoled_enabled(bool enabled) = 0;
    virtual void save_color_scheme(const std::string& scheme) = 0;
    virtual void save_dynamic_color(bool enabled) = 0;
    virtual void save_blur_effects_enabled(bool enabled) = 0;
    virtual void save_reduce_motion(bool enabled) = 0;
    virtual void save_stick_to_bottom(bool enabled) = 0;
    virtual void save_parse_inline_dollar_math(bool enabled) = 0;
    virtual void save_auto_wrap_code_blocks(bool enabled) = 0;
    virtual void save_haptics_enabled(bool enabled) = 0;
    virtual void save_detailed_token_usage(bool enabled) = 0;
    virtual void save_tool_call_display_mode(const std::string& mode) = 0;
    virtual void save_thinking_segment_display_mode(const std::string& mode) = 0;
    virtual void save_auto_expand_active_group(bool enabled) = 0;
    virtual void save_scheme_style(const std::string& style) = 0;
    virtual void save_font_preference(const std::string& preference) = 0;
    virtual void save_custom_font_path(const std::string& path) = 0;
    virtual void save_custom_font_name(const std::string& name) = 0;
    virtual void save_default_temperature(const std::optional<float>& value) = 0;
    virtual void save_default_max_tokens(const std::optional<int>& value) = 0;
    virtual void save_default_top_p(const std::optional<float>& value) = 0;
    virtual void save_default_frequency_penalty(const std::optional<float>& value) = 0;
    virtual void save_default_presence_penalty(const std::optional<float>& value) = 0;
    virtual void set_active_system_prompt_id(const std::optional<std::string>& id) = 0;
    virtual void reset_portable_settings_for_import() = 0;
    virtual void invalidate_portable_model_caches() = 0;
};

/** data/PortableSettingsArchive.kt::RestoredCustomFont. */
struct CustomFont {
    std::string path;
    std::string display_name;
};

/**
 * promptImport::resolve — injected by the call site. A null id is passed through as null; what a
 * given id resolves to is the importer's business, so the golden driver supplies the mapping.
 */
using PromptResolver = std::function<std::optional<std::string>(const std::optional<std::string>&)>;

/**
 * CustomProviderIdentityPolicy.newId entropy. Kotlin mints with UUID.randomUUID(); a port cannot
 * hide entropy behind a factory call, so the generator is a parameter and the golden stand pins
 * "generated vs retained" structurally (the driver masks ids absent from the scenario input).
 */
using IdGenerator = std::function<std::string()>;

struct RestoreOptions {
    bool replace = false;
    bool allow_legacy_secrets = false;
    std::optional<CustomFont> restored_custom_font;
    PromptResolver resolve_system_prompt_id;  // default: resolves every id to null
    IdGenerator provider_id_generator;        // required when a legacy provider id must be minted
};

struct RestoreResult {
    std::vector<std::string> warnings;
};

/** java.lang.IllegalArgumentException from the archive's `require(...)` guards. */
class IllegalArgumentException : public std::runtime_error {
public:
    using std::runtime_error::runtime_error;
    const char* kotlin_type() const noexcept { return "IllegalArgumentException"; }
};

/** PortableSettingsArchive.toJsonObject — the settings.json payload of a backup. */
json::Value to_json_object(const SettingsSnapshot& settings, bool custom_font_included);

/** PortableSettingsArchive.restoreFromJsonObject. Throws IllegalArgumentException on the
 *  cache-validation guards; writes performed before the throw stay applied. */
RestoreResult restore_from_json_object(const json::Value& obj, const SettingsSnapshot& current,
                                       SettingsSink& sink, const RestoreOptions& options);

}  // namespace agora::core::settings_archive
