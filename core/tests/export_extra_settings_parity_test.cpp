// Golden parity test for ExportExtraSettings.restoreLegacyFromJsonObject.
//
// Every scenario is replayed against the real Kotlin trace. The recorder implements the
// SettingsManager surface, appends one JSON call per save in call order, and serialises floats with
// Kotlin's Float.toString spelling. A Kotlin exception after some writes must leave those writes in
// the trace and report the same exception category.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/export_extra_settings.hpp"
#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "check.hpp"

namespace {

namespace settings = agora::core::export_extra_settings;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

const Value& required_object(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Object) {
        scenario_fail(std::string(name) + " must be an object");
    }
    return *value;
}

const Value& array_or_empty(const Value& object, std::string_view name) {
    static const Value empty = [] {
        Value value;
        value.kind = Kind::Array;
        return value;
    }();
    const Value* value = field(object, name);
    if (value == nullptr) return empty;
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    return *value;
}

bool bool_or(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a bool");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    scenario_fail(std::string(name) + " is not a bool");
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string render_string_list(const std::vector<std::string>& values) {
    std::string out = "[";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_quoted(out, values[i]);
    }
    out.push_back(']');
    return out;
}

std::string render_string_map(const settings::StringMap& values) {
    std::string out = "{";
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_quoted(out, values[i].first);
        out.push_back(':');
        append_quoted(out, values[i].second);
    }
    out.push_back('}');
    return out;
}

void append_optional(std::string& out, const char* name, const std::optional<bool>& value) {
    if (!value.has_value()) return;
    out += ",\"";
    out += name;
    out += "\":";
    out += *value ? "true" : "false";
}

void append_optional(std::string& out, const char* name, const std::optional<int>& value) {
    if (!value.has_value()) return;
    out += ",\"";
    out += name;
    out += "\":";
    out += std::to_string(*value);
}

void append_optional(std::string& out, const char* name, const std::optional<float>& value) {
    if (!value.has_value()) return;
    out += ",\"";
    out += name;
    out += "\":\"";
    out += agora::core::openai::format_kotlin_float(*value);
    out += '"';
}

void append_optional(std::string& out, const char* name, const std::optional<std::string>& value) {
    if (!value.has_value()) return;
    out += ",\"";
    out += name;
    out += "\":";
    append_quoted(out, *value);
}

std::string render_conversation(const settings::ConversationSettings& value) {
    std::string out = "{";
    bool first = true;
    const auto separator = [&]() {
        if (!first) out.push_back(',');
        first = false;
    };
    const auto begin = [&](const char* name) {
        separator();
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (value.context_window.has_value()) {
        begin("contextWindow");
        out += std::to_string(*value.context_window);
    }
    if (value.temperature.has_value()) {
        begin("temperature");
        out.push_back('"');
        out += agora::core::openai::format_kotlin_float(*value.temperature);
        out.push_back('"');
    }
    if (value.max_tokens.has_value()) {
        begin("maxTokens");
        out += std::to_string(*value.max_tokens);
    }
    if (value.top_p.has_value()) {
        begin("topP");
        out.push_back('"');
        out += agora::core::openai::format_kotlin_float(*value.top_p);
        out.push_back('"');
    }
    if (value.frequency_penalty.has_value()) {
        begin("frequencyPenalty");
        out.push_back('"');
        out += agora::core::openai::format_kotlin_float(*value.frequency_penalty);
        out.push_back('"');
    }
    if (value.presence_penalty.has_value()) {
        begin("presencePenalty");
        out.push_back('"');
        out += agora::core::openai::format_kotlin_float(*value.presence_penalty);
        out.push_back('"');
    }
    if (value.code_execution_enabled.has_value()) {
        begin("codeExecutionEnabled");
        out += *value.code_execution_enabled ? "true" : "false";
    }
    if (value.google_search_enabled.has_value()) {
        begin("googleSearchEnabled");
        out += *value.google_search_enabled ? "true" : "false";
    }
    if (value.open_ai_web_search_enabled.has_value()) {
        begin("openAiWebSearchEnabled");
        out += *value.open_ai_web_search_enabled ? "true" : "false";
    }
    if (value.thinking_enabled.has_value()) {
        begin("thinkingEnabled");
        out += *value.thinking_enabled ? "true" : "false";
    }
    if (value.thinking_level.has_value()) {
        begin("thinkingLevel");
        append_quoted(out, *value.thinking_level);
    }
    if (value.thinking_budget_enabled.has_value()) {
        begin("thinkingBudgetEnabled");
        out += *value.thinking_budget_enabled ? "true" : "false";
    }
    if (value.thinking_budget_tokens.has_value()) {
        begin("thinkingBudgetTokens");
        out += std::to_string(*value.thinking_budget_tokens);
    }
    if (value.open_ai_service_tier_enabled.has_value()) {
        begin("openAiServiceTierEnabled");
        out += *value.open_ai_service_tier_enabled ? "true" : "false";
    }
    if (value.open_ai_service_tier.has_value()) {
        begin("openAiServiceTier");
        append_quoted(out, *value.open_ai_service_tier);
    }
    if (value.web_search_enabled.has_value()) {
        begin("webSearchEnabled");
        out += *value.web_search_enabled ? "true" : "false";
    }
    if (value.shell_enabled.has_value()) {
        begin("shellEnabled");
        out += *value.shell_enabled ? "true" : "false";
    }
    if (value.low_context_mode_enabled.has_value()) {
        begin("lowContextModeEnabled");
        out += *value.low_context_mode_enabled ? "true" : "false";
    }
    out.push_back('}');
    return out;
}

/** Records the exact SettingsManager calls, mirroring the Kotlin trace JSON. */
class RecordingStore : public settings::SettingsStore {
public:
    settings::StringMap model_aliases_seed;
    std::vector<std::string> custom_models_seed;
    std::string calls;
    bool first = true;

    const settings::StringMap& model_aliases() const override { return model_aliases_seed; }
    const std::vector<std::string>& custom_models() const override { return custom_models_seed; }

    void append(const char* name, std::string value) {
        if (!first) calls.push_back(',');
        first = false;
        calls += "{\"name\":";
        append_quoted(calls, name);
        calls += ",\"value\":";
        calls += value;
        calls.push_back('}');
    }

    static std::string boolean(bool value) {
        return std::string("{\"bool\":") + (value ? "true" : "false") + "}";
    }
    static std::string integer(int value) {
        return "{\"int\":" + std::to_string(value) + "}";
    }
    static std::string floating(float value) {
        return "{\"float\":\"" + agora::core::openai::format_kotlin_float(value) + "\"}";
    }
    static std::string string(std::string_view value) {
        std::string out = "{\"string\":";
        append_quoted(out, value);
        out.push_back('}');
        return out;
    }
    static std::string string_set(const std::vector<std::string>& values) {
        return "{\"strings\":" + render_string_list(values) + "}";
    }
    static std::string string_map(const settings::StringMap& values) {
        return "{\"map\":" + render_string_map(values) + "}";
    }

    void save_image_transcription_enabled(bool value) override {
        append("saveImageTranscriptionEnabled", boolean(value));
    }
    void save_image_transcription_enabled_models(
        const std::vector<std::string>& models) override {
        append("saveImageTranscriptionEnabledModels", string_set(models));
    }
    void save_image_transcription_model(const std::string& model) override {
        append("saveImageTranscriptionModel", string(model));
    }
    void save_image_transcription_batch_size(int size) override {
        append("saveImageTranscriptionBatchSize", integer(size));
    }
    void save_image_transcription_prompt(const std::string& prompt) override {
        append("saveImageTranscriptionPrompt", string(prompt));
    }
    void save_web_search_num_results(int count) override {
        append("saveWebSearchNumResults", integer(count));
    }
    void save_search_context_window(int window) override {
        append("saveSearchContextWindow", integer(window));
    }
    void save_search_match_limit(int limit) override {
        append("saveSearchMatchLimit", integer(limit));
    }
    void save_default_temperature(float value) override {
        append("saveDefaultTemperature", floating(value));
    }
    void save_default_max_tokens(int value) override {
        append("saveDefaultMaxTokens", integer(value));
    }
    void save_default_top_p(float value) override { append("saveDefaultTopP", floating(value)); }
    void save_default_frequency_penalty(float value) override {
        append("saveDefaultFrequencyPenalty", floating(value));
    }
    void save_default_presence_penalty(float value) override {
        append("saveDefaultPresencePenalty", floating(value));
    }
    void save_conversation_settings(const std::string& conversation_id,
                                    const settings::ConversationSettings& value) override {
        std::string rendered = "{\"conversationId\":";
        append_quoted(rendered, conversation_id);
        rendered += ",\"settings\":";
        rendered += render_conversation(value);
        rendered.push_back('}');
        append("saveConversationSettings", std::move(rendered));
    }
    void save_show_documentation_fab(bool value) override {
        append("saveShowDocumentationFab", boolean(value));
    }
    void save_proxy_enabled(bool value) override { append("saveProxyEnabled", boolean(value)); }
    void save_proxy_type(const std::string& value) override { append("saveProxyType", string(value)); }
    void save_proxy_host(const std::string& value) override { append("saveProxyHost", string(value)); }
    void save_proxy_port(const std::string& value) override { append("saveProxyPort", string(value)); }
    void save_proxy_username(const std::string& value) override {
        append("saveProxyUsername", string(value));
    }
    void save_proxy_password(const std::string& value) override {
        append("saveProxyPassword", string(value));
    }
    void save_proxy_bypass(const std::string& value) override {
        append("saveProxyBypass", string(value));
    }
    void save_theme_mode(const std::string& value) override {
        append("saveThemeMode", string(value));
    }
    void save_color_scheme(const std::string& value) override {
        append("saveColorScheme", string(value));
    }
    void save_dynamic_color(bool value) override { append("saveDynamicColor", boolean(value)); }
    void save_blur_effects_enabled(bool value) override {
        append("saveBlurEffectsEnabled", boolean(value));
    }
    void save_reduce_motion(bool value) override { append("saveReduceMotion", boolean(value)); }
    void save_open_ai_service_tier_enabled(bool value) override {
        append("saveOpenAiServiceTierEnabled", boolean(value));
    }
    void save_open_ai_service_tier(const std::string& value) override {
        append("saveOpenAiServiceTier", string(value));
    }
    void save_haptics_enabled(bool value) override { append("saveHapticsEnabled", boolean(value)); }
    void save_detailed_token_usage(bool value) override {
        append("saveDetailedTokenUsage", boolean(value));
    }
    void save_auto_expand_active_group(bool value) override {
        append("saveAutoExpandActiveGroup", boolean(value));
    }
    void save_scheme_style(const std::string& value) override {
        append("saveSchemeStyle", string(value));
    }
    void save_font_preference(const std::string& value) override {
        append("saveFontPreference", string(value));
    }
    void save_auto_update_check(bool value) override {
        append("saveAutoUpdateCheck", boolean(value));
    }
    void save_automation_tools_enabled(bool value) override {
        append("saveAutomationToolsEnabled", boolean(value));
    }
    void save_exact_execution_enabled(bool value) override {
        append("saveExactExecutionEnabled", boolean(value));
    }
    void save_automation_wake_lock_enabled(bool value) override {
        append("saveAutomationWakeLockEnabled", boolean(value));
    }
    void save_model_aliases(const settings::StringMap& aliases) override {
        append("saveModelAliases", string_map(aliases));
    }
    void save_custom_models(const std::vector<std::string>& models) override {
        append("saveCustomModels", string_set(models));
    }
};

settings::StringMap parse_aliases(const Value& root) {
    settings::StringMap aliases;
    const Value* value = field(root, "modelAliases");
    if (value == nullptr) return aliases;
    if (value->kind != Kind::Object) scenario_fail("modelAliases must be an object");
    for (const Member& member : value->members) {
        if (member.value.kind != Kind::String) {
            scenario_fail("modelAliases values must be strings");
        }
        aliases.emplace_back(member.key, member.value.text);
    }
    return aliases;
}

std::vector<std::string> parse_models(const Value& root) {
    std::vector<std::string> models;
    for (const Value& value : array_or_empty(root, "customModels").elements) {
        if (value.kind != Kind::String) scenario_fail("customModels must contain strings");
        models.push_back(value.text);
    }
    return models;
}

std::vector<std::string> parse_allowed(const Value& root) {
    std::vector<std::string> ids;
    for (const Value& value : array_or_empty(root, "allowedConversationIds").elements) {
        if (value.kind != Kind::String) {
            scenario_fail("allowedConversationIds must contain strings");
        }
        ids.push_back(value.text);
    }
    return ids;
}

std::string render_trace(const RecordingStore& store, const std::string* error_type) {
    std::string trace = "{\"calls\":[" + store.calls + "]";
    if (error_type != nullptr) {
        trace += ",\"error\":";
        append_quoted(trace, *error_type);
    }
    trace.push_back('}');
    return trace;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(stem + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const Value& object = required_object(root, "object");

    RecordingStore store;
    store.model_aliases_seed = parse_aliases(root);
    store.custom_models_seed = parse_models(root);

    settings::RestoreOptions options;
    options.replace = bool_or(root, "replace", false);
    options.allow_secrets = bool_or(root, "allowSecrets", false);
    options.allowed_conversation_ids = parse_allowed(root);

    std::string error_type;
    try {
        settings::restore_legacy_from_json_object(object, store, options);
    } catch (const settings::KotlinException& error) {
        error_type = error.kotlin_type();
    }
    return render_trace(store, error_type.empty() ? nullptr : &error_type);
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(export_extra_settings_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_EXPORT_EXTRA_SETTINGS_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".json") scenarios.push_back(entry.path());
    }
    std::sort(scenarios.begin(), scenarios.end());
    CHECK(!scenarios.empty());

    int checked = 0;
    for (const std::filesystem::path& scenario : scenarios) {
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
        CHECK(!expected.empty());
        const std::string actual = run_scenario(scenario);
        if (actual != expected) {
            throw agora::core::test::Failure(scenario.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
