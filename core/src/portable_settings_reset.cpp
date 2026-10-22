// Port of data/PortableSettingsReset.kt.
//
// The list of removed preferences is generated from the Kotlin policy (the stand's
// gen_reset_keys.py writes it into the header), so the two cannot drift apart.
#include "agora/core/portable_settings_reset.hpp"

namespace agora::core::settings_reset {

namespace {

// GENERATED from data/PortableSettingsReset.kt -- do not edit.
const std::vector<std::string>& removed_names() {
    static const std::vector<std::string> names = {
        "selected_model",
        "custom_models",
        "enabled_models",
        "active_system_prompt_id",
        "model_aliases_json",
        "context_token_budget",
        "max_context_window",
        "visualize_context_rollout",
        "context_compact_enabled",
        "context_compact_model",
        "context_compact_prompt",
        "context_compact_retain_count",
        "context_compact_preserve_system_prompt",
        "context_compact_threshold_percent",
        "code_execution_enabled",
        "google_search_enabled",
        "thinking_enabled",
        "thinking_level",
        "thinking_budget_enabled",
        "thinking_budget_tokens",
        "openai_service_tier_enabled",
        "openai_service_tier",
        "openai_responses_api_enabled",
        "anthropic_cache_enabled",
        "anthropic_cache_ttl",
        "provider_base_urls",
        "title_generation_enabled",
        "title_generation_model",
        "title_generation_prompt",
        "title_generation_notifications_enabled",
        "image_transcription_enabled",
        "image_transcription_enabled_models",
        "image_transcription_model",
        "image_transcription_batch_size",
        "image_transcription_prompt",
        "access_past_conversations",
        "access_saved_memories",
        "access_active_memory",
        "access_skills",
        "access_skills_modify",
        "rag_search_enabled",
        "model_search_method",
        "manual_search_method",
        "app_language",
        "web_search_enabled",
        "web_search_provider",
        "web_search_num_results",
        "web_search_base_url",
        "image_gen_enabled",
        "image_gen_model",
        "image_gen_size",
        "search_context_window",
        "search_match_limit",
        "rag_threshold",
        "auto_cache_enabled",
        "show_uncached_notification",
        "auto_update_check",
        "custom_providers_json",
        "shell_enabled",
        "automation_tools_enabled",
        "exact_execution_enabled",
        "automation_wake_lock_enabled",
        "proxy_enabled",
        "proxy_type",
        "proxy_host",
        "proxy_port",
        "proxy_username",
        "proxy_bypass",
        "shell_confirm_enabled",
        "theme_mode",
        "amoled_enabled",
        "color_scheme",
        "dynamic_color",
        "blur_effects_enabled",
        "reduce_motion",
        "stick_to_bottom",
        "parse_inline_dollar_math",
        "auto_wrap_code_blocks",
        "haptics_enabled",
        "detailed_token_usage",
        "tool_call_display_mode",
        "thinking_segment_display_mode",
        "auto_expand_active_group",
        "scheme_style",
        "font_preference",
        "show_documentation_fab",
        "default_temperature",
        "default_max_tokens",
        "default_top_p",
        "default_frequency_penalty",
        "default_presence_penalty",
        "available_models_json",
        "custom_endpoint_resolutions_json",
        "last_models_fetch_fingerprint",
    };
    return names;
}

}  // namespace

const std::vector<std::string>& removed_preference_names() { return removed_names(); }

void clear_portable_settings(PreferenceStore& store) {
    for (const std::string& name : removed_names()) {
        if (name == kModelProviderNamesKey) continue;  // rewritten below, not removed
        store.remove(name);
    }
    store.set_raw(kModelProviderNamesKey, kEmptyJsonObject);
}

}  // namespace agora::core::settings_reset
