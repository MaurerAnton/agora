// Golden parity test for PortableSettingsArchive (export + restore).
//
// Each scenario replays the real Kotlin trace: the export half must reproduce the exact
// settings.json text (kotlinx encodeDefaults=false, Float.toString spelling, StringOps escaping),
// the restore half must reproduce the ordered save-call log, the warnings, the error category and
// message, and the "generated vs retained" custom-provider id structure (ids that appear nowhere
// in the scenario input are masked, exactly like the Kotlin driver).
#include <algorithm>
#include <cctype>
#include <cerrno>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/portable_settings_archive.hpp"
#include "check.hpp"

namespace {

namespace archive = agora::core::settings_archive;
namespace json = agora::core::json;
namespace backup = agora::core::native_backup;

using json::Kind;
using json::Member;
using json::Value;

/** Scenario snapshot numbers are plain JSON literals; full-match strtof/strtol is enough. */
std::optional<int> parse_int_for_scenario(std::string_view text) {
    const std::string copy(text);
    char* end = nullptr;
    errno = 0;
    const long value = std::strtol(copy.c_str(), &end, 10);
    if (end != copy.c_str() + copy.size()) return std::nullopt;
    if (value < std::numeric_limits<int>::min() || value > std::numeric_limits<int>::max()) {
        return std::nullopt;
    }
    return static_cast<int>(value);
}

std::optional<float> parse_float_for_scenario(std::string_view text) {
    const std::string copy(text);
    char* end = nullptr;
    const float value = std::strtof(copy.c_str(), &end);
    if (end != copy.c_str() + copy.size()) return std::nullopt;
    return value;
}

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
        if (entry.key == name) found = &entry.value;  // last duplicate wins
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

// ------------------------------------------------------------------ scenario -> SettingsSnapshot

std::string read_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> read_nullable_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " must be present");
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

bool read_bool(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " must be a bool");
    std::string lowered;
    if (value != nullptr) lowered = value->text;
    for (char& c : lowered) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    if ((value->kind == Kind::Literal || value->kind == Kind::String) && lowered == "true") {
        return true;
    }
    if ((value->kind == Kind::Literal || value->kind == Kind::String) && lowered == "false") {
        return false;
    }
    scenario_fail(std::string(name) + " is not a bool");
}

std::optional<int> read_nullable_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " must be present");
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be an int");
    const std::optional<int> parsed = parse_int_for_scenario(value->text);
    if (!parsed.has_value()) scenario_fail(std::string(name) + " is not an int");
    return parsed;
}

int read_int(const Value& object, std::string_view name) {
    const std::optional<int> value = read_nullable_int(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " must not be null");
    return *value;
}

std::optional<float> read_nullable_float(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " must be present");
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a float");
    const std::optional<float> parsed = parse_float_for_scenario(value->text);
    if (!parsed.has_value()) scenario_fail(std::string(name) + " is not a float");
    return parsed;
}

float read_float(const Value& object, std::string_view name) {
    const std::optional<float> value = read_nullable_float(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " must not be null");
    return *value;
}

std::vector<std::string> read_string_set(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<std::string> out;  // LinkedHashSet: first occurrence wins
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail(std::string(name) + " needs strings");
        if (std::find(out.begin(), out.end(), element.text) == out.end()) {
            out.push_back(element.text);
        }
    }
    return out;
}

archive::StringMap read_string_map(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Object) {
        scenario_fail(std::string(name) + " must be an object");
    }
    archive::StringMap out;  // LinkedHashMap: last value, first position
    for (const Member& member : value->members) {
        if (member.value.kind != Kind::String) {
            scenario_fail(std::string(name) + " values must be strings");
        }
        bool replaced = false;
        for (auto& entry : out) {
            if (entry.first == member.key) {
                entry.second = member.value.text;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.emplace_back(member.key, member.value.text);
    }
    return out;
}

archive::BoolMap read_bool_map(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Object) {
        scenario_fail(std::string(name) + " must be an object");
    }
    archive::BoolMap out;
    for (const Member& member : value->members) {
        bool flag;
        const std::string& text = member.value.text;
        if ((member.value.kind == Kind::Literal || member.value.kind == Kind::String) &&
            (text == "true" || text == "TRUE" || text == "True")) {
            flag = true;
        } else if ((member.value.kind == Kind::Literal || member.value.kind == Kind::String) &&
                   (text == "false" || text == "FALSE" || text == "False")) {
            flag = false;
        } else {
            scenario_fail(std::string(name) + " values must be bools");
        }
        bool replaced = false;
        for (auto& entry : out) {
            if (entry.first == member.key) {
                entry.second = flag;
                replaced = true;
                break;
            }
        }
        if (!replaced) out.emplace_back(member.key, flag);
    }
    return out;
}

std::vector<backup::EmbeddingModelConfig> read_embedding_list(const Value& object,
                                                              std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<backup::EmbeddingModelConfig> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail(std::string(name) + " entries must be objects");
        backup::EmbeddingModelConfig model;
        if (const Value* id = field(element, "id")) {
            if (id->kind != Kind::String) scenario_fail(std::string(name) + ".id must be a string");
            model.id = id->text;
        }
        if (const Value* name_value = field(element, "name");
            name_value != nullptr && name_value->kind == Kind::String) {
            model.name = name_value->text;
        }
        if (const Value* type = field(element, "type")) {
            if (type->kind != Kind::String) scenario_fail(std::string(name) + ".type must be a string");
            model.type = type->text == "LOCAL" ? backup::EmbeddingModelType::Local
                                               : backup::EmbeddingModelType::Remote;
        }
        if (const Value* remote_name = field(element, "remoteModelName")) {
            if (remote_name->kind != Kind::String) scenario_fail("remoteModelName must be a string");
            model.remote_model_name = remote_name->text;
        }
        if (const Value* remote_url = field(element, "remoteBaseUrl")) {
            if (remote_url->kind != Kind::String) scenario_fail("remoteBaseUrl must be a string");
            model.remote_base_url = remote_url->text;
        }
        if (const Value* remote_key = field(element, "remoteApiKey")) {
            if (remote_key->kind != Kind::String) scenario_fail("remoteApiKey must be a string");
            model.remote_api_key = remote_key->text;
        }
        if (const Value* local_path = field(element, "localFilePath")) {
            if (local_path->kind != Kind::String) scenario_fail("localFilePath must be a string");
            model.local_file_path = local_path->text;
        }
        if (const Value* batch = field(element, "batchSize")) {
            if (batch->kind != Kind::Literal) scenario_fail("batchSize must be an int");
            model.batch_size = parse_int_for_scenario(batch->text).value_or(8);
        }
        out.push_back(model);
    }
    return out;
}

std::vector<backup::ShellDeviceConfig> read_shell_list(const Value& object,
                                                       std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<backup::ShellDeviceConfig> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail(std::string(name) + " entries must be objects");
        backup::ShellDeviceConfig device;
        if (const Value* id = field(element, "id")) device.id = id->text;
        if (const Value* device_name = field(element, "name")) device.name = device_name->text;
        auto text_or = [&](std::string_view key, std::string* out_value) {
            if (const Value* member = field(element, key)) {
                if (member->kind != Kind::String) scenario_fail("shell field must be a string");
                *out_value = member->text;
            }
        };
        text_or("description", &device.description);
        text_or("type", &device.type);
        text_or("serverUrl", &device.server_url);
        text_or("apiKey", &device.api_key);
        text_or("conchPublicKey", &device.conch_public_key);
        text_or("sshHost", &device.ssh_host);
        if (const Value* port = field(element, "sshPort")) {
            device.ssh_port = parse_int_for_scenario(port->text).value_or(22);
        }
        text_or("sshUser", &device.ssh_user);
        text_or("sshPassword", &device.ssh_password);
        text_or("sshHostKey", &device.ssh_host_key);
        out.push_back(device);
    }
    return out;
}

std::vector<backup::McpServerConfig> read_mcp_list(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<backup::McpServerConfig> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail(std::string(name) + " entries must be objects");
        backup::McpServerConfig server;
        if (const Value* id = field(element, "id")) server.id = id->text;
        if (const Value* server_name = field(element, "name")) server.name = server_name->text;
        if (const Value* enabled = field(element, "enabled")) {
            if (enabled->kind == Kind::Literal && enabled->text == "false") {
                server.enabled = false;
            } else if (enabled->kind == Kind::Literal && enabled->text == "true") {
                server.enabled = true;
            } else {
                scenario_fail("enabled must be a bool");
            }
        }
        if (const Value* url = field(element, "url")) server.url = url->text;
        if (const Value* transport = field(element, "transport")) server.transport = transport->text;
        if (const Value* headers = field(element, "headers")) {
            if (headers->kind != Kind::Object) scenario_fail("headers must be an object");
            for (const Member& member : headers->members) {
                server.headers.emplace_back(member.key, member.value.text);
            }
        }
        if (const Value* tools = field(element, "disabledTools")) {
            if (tools->kind != Kind::Array) scenario_fail("disabledTools must be an array");
            for (const Value& tool : tools->elements) {
                server.disabled_tools.push_back(tool.text);
            }
        }
        out.push_back(server);
    }
    return out;
}

std::vector<archive::CustomProviderConfig> read_provider_list(const Value& object,
                                                              std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Array) {
        scenario_fail(std::string(name) + " must be an array");
    }
    std::vector<archive::CustomProviderConfig> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail(std::string(name) + " entries must be objects");
        archive::CustomProviderConfig provider;
        if (const Value* provider_name = field(element, "name")) provider.name = provider_name->text;
        if (const Value* protocol = field(element, "protocol")) {
            if (protocol->kind != Kind::String) scenario_fail("protocol must be a string");
            provider.protocol = protocol->text;
        }
        if (const Value* id = field(element, "id")) provider.id = id->text;
        if (const Value* legacy = field(element, "legacyNames");
            legacy != nullptr && legacy->kind == Kind::Array) {
            for (const Value& entry : legacy->elements) {
                provider.legacy_names.push_back(entry.text);
            }
        }
        if (const Value* responses = field(element, "responsesApiEnabled")) {
            if (responses->kind != Kind::Literal) scenario_fail("responsesApiEnabled must be bool");
            provider.responses_api_enabled = responses->text == "true";
        }
        if (const Value* cache = field(element, "anthropicCacheEnabled")) {
            if (cache->kind != Kind::Literal) scenario_fail("anthropicCacheEnabled must be bool");
            provider.anthropic_cache_enabled = cache->text == "true";
        }
        if (const Value* ttl = field(element, "anthropicCacheTtl")) {
            if (ttl->kind != Kind::String) scenario_fail("anthropicCacheTtl must be a string");
            provider.anthropic_cache_ttl = ttl->text;
        }
        out.push_back(provider);
    }
    return out;
}

archive::SettingsSnapshot read_snapshot(const Value& root) {
    const Value& s = required_object(root, "settings");
    archive::SettingsSnapshot snapshot;
    snapshot.selected_model = read_string(s, "selectedModel");
    snapshot.custom_models = read_string_set(s, "customModels");
    snapshot.enabled_models = read_string_set(s, "enabledModels");
    snapshot.model_aliases = read_string_map(s, "modelAliases");
    snapshot.model_provider_names = read_bool_map(s, "modelProviderNames");
    snapshot.max_context_window = read_int(s, "maxContextWindow");
    snapshot.visualize_context_rollout = read_bool(s, "visualizeContextRollout");
    snapshot.context_compact_enabled = read_bool(s, "contextCompactEnabled");
    snapshot.context_compact_model = read_nullable_string(s, "contextCompactModel");
    snapshot.context_compact_prompt = read_string(s, "contextCompactPrompt");
    snapshot.context_compact_retain_count = read_int(s, "contextCompactRetainCount");
    snapshot.context_compact_preserve_system_prompt =
        read_bool(s, "contextCompactPreserveSystemPrompt");
    snapshot.context_compact_threshold_percent = read_int(s, "contextCompactThresholdPercent");
    snapshot.code_execution_enabled = read_bool(s, "codeExecutionEnabled");
    snapshot.google_search_enabled = read_bool(s, "googleSearchEnabled");
    snapshot.thinking_enabled = read_bool(s, "thinkingEnabled");
    snapshot.thinking_level = read_string(s, "thinkingLevel");
    snapshot.thinking_budget_enabled = read_bool(s, "thinkingBudgetEnabled");
    snapshot.thinking_budget_tokens = read_int(s, "thinkingBudgetTokens");
    snapshot.open_ai_service_tier_enabled = read_bool(s, "openAiServiceTierEnabled");
    snapshot.open_ai_service_tier = read_string(s, "openAiServiceTier");
    snapshot.open_ai_responses_api_enabled = read_bool(s, "openAiResponsesApiEnabled");
    snapshot.anthropic_cache_enabled = read_bool(s, "anthropicCacheEnabled");
    snapshot.anthropic_cache_ttl = read_string(s, "anthropicCacheTtl");
    snapshot.provider_base_urls = read_string_map(s, "providerBaseUrls");
    snapshot.title_generation_enabled = read_bool(s, "titleGenerationEnabled");
    snapshot.title_generation_model = read_nullable_string(s, "titleGenerationModel");
    snapshot.title_generation_prompt = read_string(s, "titleGenerationPrompt");
    snapshot.title_generation_notifications_enabled =
        read_bool(s, "titleGenerationNotificationsEnabled");
    snapshot.access_past_conversations = read_bool(s, "accessPastConversations");
    snapshot.access_saved_memories = read_bool(s, "accessSavedMemories");
    snapshot.access_active_memory = read_bool(s, "accessActiveMemory");
    snapshot.access_skills = read_bool(s, "accessSkills");
    snapshot.rag_search_enabled = read_bool(s, "ragSearchEnabled");
    snapshot.model_search_method = read_string(s, "modelSearchMethod");
    snapshot.manual_search_method = read_string(s, "manualSearchMethod");
    snapshot.embedding_models = read_embedding_list(s, "embeddingModels");
    snapshot.active_embedding_model_id = read_string(s, "activeEmbeddingModelId");
    snapshot.app_language = read_string(s, "appLanguage");
    snapshot.web_search_enabled = read_bool(s, "webSearchEnabled");
    snapshot.web_search_provider = read_string(s, "webSearchProvider");
    snapshot.web_search_num_results = read_int(s, "webSearchNumResults");
    snapshot.web_search_base_url = read_string(s, "webSearchBaseUrl");
    snapshot.image_gen_enabled = read_bool(s, "imageGenEnabled");
    snapshot.image_gen_model = read_nullable_string(s, "imageGenModel");
    snapshot.image_gen_size = read_string(s, "imageGenSize");
    snapshot.search_context_window = read_int(s, "searchContextWindow");
    snapshot.search_match_limit = read_int(s, "searchMatchLimit");
    snapshot.rag_threshold = read_float(s, "ragThreshold");
    snapshot.auto_cache_enabled = read_bool(s, "autoCacheEnabled");
    snapshot.show_uncached_notification = read_bool(s, "showUncachedNotification");
    snapshot.auto_update_check = read_bool(s, "autoUpdateCheck");
    snapshot.image_transcription_enabled = read_bool(s, "imageTranscriptionEnabled");
    snapshot.image_transcription_enabled_models =
        read_string_set(s, "imageTranscriptionEnabledModels");
    snapshot.image_transcription_model = read_nullable_string(s, "imageTranscriptionModel");
    snapshot.image_transcription_batch_size = read_int(s, "imageTranscriptionBatchSize");
    snapshot.image_transcription_prompt = read_string(s, "imageTranscriptionPrompt");
    snapshot.shell_enabled = read_bool(s, "shellEnabled");
    snapshot.shell_confirm_enabled = read_bool(s, "shellConfirmEnabled");
    snapshot.ask_user_enabled = read_bool(s, "askUserEnabled");
    snapshot.shell_devices = read_shell_list(s, "shellDevices");
    snapshot.automation_tools_enabled = read_bool(s, "automationToolsEnabled");
    snapshot.exact_execution_enabled = read_bool(s, "exactExecutionEnabled");
    snapshot.automation_wake_lock_enabled = read_bool(s, "automationWakeLockEnabled");
    snapshot.custom_providers = read_provider_list(s, "customProviders");
    snapshot.mcp_servers = read_mcp_list(s, "mcpServers");
    snapshot.proxy_enabled = read_bool(s, "proxyEnabled");
    snapshot.proxy_type = read_string(s, "proxyType");
    snapshot.proxy_host = read_string(s, "proxyHost");
    snapshot.proxy_port = read_string(s, "proxyPort");
    snapshot.proxy_username = read_string(s, "proxyUsername");
    snapshot.proxy_bypass = read_string(s, "proxyBypass");
    snapshot.show_documentation_fab = read_bool(s, "showDocumentationFab");
    snapshot.theme_mode = read_string(s, "themeMode");
    snapshot.amoled_enabled = read_bool(s, "amoledEnabled");
    snapshot.color_scheme = read_string(s, "colorScheme");
    snapshot.dynamic_color = read_bool(s, "dynamicColor");
    snapshot.blur_effects_enabled = read_bool(s, "blurEffectsEnabled");
    snapshot.reduce_motion = read_bool(s, "reduceMotion");
    snapshot.stick_to_bottom = read_bool(s, "stickToBottom");
    snapshot.parse_inline_dollar_math = read_bool(s, "parseInlineDollarMath");
    snapshot.auto_wrap_code_blocks = read_bool(s, "autoWrapCodeBlocks");
    snapshot.haptics_enabled = read_bool(s, "hapticsEnabled");
    snapshot.detailed_token_usage = read_bool(s, "detailedTokenUsage");
    snapshot.tool_call_display_mode = read_string(s, "toolCallDisplayMode");
    snapshot.thinking_segment_display_mode = read_string(s, "thinkingSegmentDisplayMode");
    snapshot.auto_expand_active_group = read_bool(s, "autoExpandActiveGroup");
    snapshot.scheme_style = read_string(s, "schemeStyle");
    snapshot.font_preference = read_string(s, "fontPreference");
    snapshot.custom_font_name = read_string(s, "customFontName");
    snapshot.custom_font_path = read_string(s, "customFontPath");
    snapshot.default_temperature = read_nullable_float(s, "defaultTemperature");
    snapshot.default_max_tokens = read_nullable_int(s, "defaultMaxTokens");
    snapshot.default_top_p = read_nullable_float(s, "defaultTopP");
    snapshot.default_frequency_penalty = read_nullable_float(s, "defaultFrequencyPenalty");
    snapshot.default_presence_penalty = read_nullable_float(s, "defaultPresencePenalty");
    snapshot.active_system_prompt_id = read_nullable_string(s, "activeSystemPromptId");
    return snapshot;
}

// ---------------------------------------------------------------------- recording SettingsSink

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string quoted(std::string_view text) {
    std::string out;
    append_quoted(out, text);
    return out;
}

class RecordingStore : public archive::SettingsSink {
public:
    std::string calls;
    bool first = true;

    void append(const char* name, const std::string& value) {
        if (!first) calls.push_back(',');
        first = false;
        calls += "{\"name\":";
        append_quoted(calls, name);
        calls += ",\"value\":";
        calls += value;
        calls.push_back('}');
    }

    static std::string boolean_value(bool value) {
        return std::string("{\"bool\":") + (value ? "true" : "false") + "}";
    }
    static std::string int_value(int value) {
        return "{\"int\":" + std::to_string(value) + "}";
    }
    static std::string float_value(const std::optional<float>& value) {
        if (!value.has_value()) return "{\"float\":null}";
        return "{\"float\":\"" + agora::core::openai::format_kotlin_float(*value) + "\"}";
    }
    static std::string string_value(const std::optional<std::string>& value) {
        if (!value.has_value()) return "{\"string\":null}";
        std::string out = "{\"string\":";
        append_quoted(out, *value);
        out.push_back('}');
        return out;
    }
    static std::string strings_value(const std::vector<std::string>& values) {
        std::string out = "{\"strings\":[";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out.push_back(',');
            append_quoted(out, values[i]);
        }
        out += "]}";
        return out;
    }
    static std::string map_value(const archive::StringMap& values) {
        std::string out = "{\"map\":{";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) out.push_back(',');
            append_quoted(out, values[i].first);
            out.push_back(':');
            append_quoted(out, values[i].second);
        }
        out += "}}";
        return out;
    }
    static std::string embedding_value(const std::vector<backup::EmbeddingModelConfig>& models) {
        std::string out = "{\"embeddingModels\":[";
        for (std::size_t i = 0; i < models.size(); ++i) {
            if (i != 0) out.push_back(',');
            const auto& model = models[i];
            out += "{\"id\":";
            append_quoted(out, model.id);
            out += ",\"name\":";
            append_quoted(out, model.name);
            out += ",\"type\":";
            append_quoted(out, model.type == backup::EmbeddingModelType::Remote ? "REMOTE"
                                                                                : "LOCAL");
            out += ",\"remoteModelName\":";
            append_quoted(out, model.remote_model_name);
            out += ",\"remoteBaseUrl\":";
            append_quoted(out, model.remote_base_url);
            out += ",\"remoteApiKey\":";
            append_quoted(out, model.remote_api_key);
            out += ",\"localFilePath\":";
            append_quoted(out, model.local_file_path);
            out += ",\"batchSize\":" + std::to_string(model.batch_size) + "}";
        }
        out += "]}";
        return out;
    }
    static std::string shell_value(const std::vector<backup::ShellDeviceConfig>& devices) {
        std::string out = "{\"shellDevices\":[";
        for (std::size_t i = 0; i < devices.size(); ++i) {
            if (i != 0) out.push_back(',');
            const auto& device = devices[i];
            out += "{\"id\":";
            append_quoted(out, device.id);
            out += ",\"name\":";
            append_quoted(out, device.name);
            out += ",\"description\":";
            append_quoted(out, device.description);
            out += ",\"type\":";
            append_quoted(out, device.type);
            out += ",\"serverUrl\":";
            append_quoted(out, device.server_url);
            out += ",\"apiKey\":";
            append_quoted(out, device.api_key);
            out += ",\"conchPublicKey\":";
            append_quoted(out, device.conch_public_key);
            out += ",\"sshHost\":";
            append_quoted(out, device.ssh_host);
            out += ",\"sshPort\":" + std::to_string(device.ssh_port);
            out += ",\"sshUser\":";
            append_quoted(out, device.ssh_user);
            out += ",\"sshPassword\":";
            append_quoted(out, device.ssh_password);
            out += ",\"sshHostKey\":";
            append_quoted(out, device.ssh_host_key);
            out += "}";
        }
        out += "]}";
        return out;
    }
    static std::string mcp_value(const std::vector<backup::McpServerConfig>& servers) {
        std::string out = "{\"mcpServers\":[";
        for (std::size_t i = 0; i < servers.size(); ++i) {
            if (i != 0) out.push_back(',');
            const auto& server = servers[i];
            out += "{\"id\":";
            append_quoted(out, server.id);
            out += ",\"name\":";
            append_quoted(out, server.name);
            out += ",\"enabled\":" + std::string(server.enabled ? "true" : "false");
            out += ",\"url\":";
            append_quoted(out, server.url);
            out += ",\"transport\":";
            append_quoted(out, server.transport == "sse" ? "SSE" : "STREAMABLE_HTTP");
            out += ",\"headers\":{";
            for (std::size_t h = 0; h < server.headers.size(); ++h) {
                if (h != 0) out.push_back(',');
                append_quoted(out, server.headers[h].first);
                out.push_back(':');
                append_quoted(out, server.headers[h].second);
            }
            out += "},\"disabledTools\":[";
            for (std::size_t t = 0; t < server.disabled_tools.size(); ++t) {
                if (t != 0) out.push_back(',');
                append_quoted(out, server.disabled_tools[t]);
            }
            out += "]}";
        }
        out += "]}";
        return out;
    }
    static std::string provider_value(
        const std::vector<archive::CustomProviderConfig>& providers) {
        std::string out = "{\"customProviders\":[";
        for (std::size_t i = 0; i < providers.size(); ++i) {
            if (i != 0) out.push_back(',');
            const auto& provider = providers[i];
            out += "{\"name\":";
            append_quoted(out, provider.name);
            out += ",\"protocol\":";
            append_quoted(out, provider.protocol);
            out += ",\"id\":";
            append_quoted(out, provider.id);
            out += ",\"legacyNames\":[";
            for (std::size_t n = 0; n < provider.legacy_names.size(); ++n) {
                if (n != 0) out.push_back(',');
                append_quoted(out, provider.legacy_names[n]);
            }
            out += "],\"responsesApiEnabled\":";
            out += provider.responses_api_enabled ? "true" : "false";
            out += ",\"anthropicCacheEnabled\":";
            out += provider.anthropic_cache_enabled ? "true" : "false";
            out += ",\"anthropicCacheTtl\":";
            append_quoted(out, provider.anthropic_cache_ttl);
            out += "}";
        }
        out += "]}";
        return out;
    }

    void save_selected_model(const std::string& model) override {
        append("saveSelectedModel", string_value(model));
    }
    void save_custom_models(const std::vector<std::string>& models) override {
        append("saveCustomModels", strings_value(models));
    }
    void save_enabled_models(const std::vector<std::string>& models) override {
        append("saveEnabledModels", strings_value(models));
    }
    void save_model_aliases(const archive::StringMap& aliases) override {
        append("saveModelAliases", map_value(aliases));
    }
    void save_model_provider_names(const archive::BoolMap& values, bool replace) override {
        std::string map = "{\"map\":{";
        for (std::size_t i = 0; i < values.size(); ++i) {
            if (i != 0) map.push_back(',');
            append_quoted(map, values[i].first);
            map.push_back(':');
            map += values[i].second ? "true" : "false";
        }
        map += "},\"bool\":";
        map += replace ? "true" : "false";
        map.push_back('}');
        append("saveModelProviderNames", map);
    }
    void save_max_context_window(int window) override {
        append("saveMaxContextWindow", int_value(window));
    }
    void save_visualize_context_rollout(bool enabled) override {
        append("saveVisualizeContextRollout", boolean_value(enabled));
    }
    void save_context_compact_enabled(bool enabled) override {
        append("saveContextCompactEnabled", boolean_value(enabled));
    }
    void save_context_compact_model(const std::optional<std::string>& model) override {
        append("saveContextCompactModel", string_value(model));
    }
    void save_context_compact_prompt(const std::string& prompt) override {
        append("saveContextCompactPrompt", string_value(std::optional<std::string>(prompt)));
    }
    void save_context_compact_retain_count(int count) override {
        append("saveContextCompactRetainCount", int_value(count));
    }
    void save_context_compact_preserve_system_prompt(bool enabled) override {
        append("saveContextCompactPreserveSystemPrompt", boolean_value(enabled));
    }
    void save_context_compact_threshold_percent(int percent) override {
        append("saveContextCompactThresholdPercent", int_value(percent));
    }
    void save_code_execution_enabled(bool enabled) override {
        append("saveCodeExecutionEnabled", boolean_value(enabled));
    }
    void save_google_search_enabled(bool enabled) override {
        append("saveGoogleSearchEnabled", boolean_value(enabled));
    }
    void save_thinking_enabled(bool enabled) override {
        append("saveThinkingEnabled", boolean_value(enabled));
    }
    void save_thinking_level(const std::string& level) override {
        append("saveThinkingLevel", string_value(std::optional<std::string>(level)));
    }
    void save_thinking_budget_enabled(bool enabled) override {
        append("saveThinkingBudgetEnabled", boolean_value(enabled));
    }
    void save_thinking_budget_tokens(int tokens) override {
        append("saveThinkingBudgetTokens", int_value(tokens));
    }
    void save_open_ai_service_tier_enabled(bool enabled) override {
        append("saveOpenAiServiceTierEnabled", boolean_value(enabled));
    }
    void save_open_ai_service_tier(const std::string& tier) override {
        append("saveOpenAiServiceTier", string_value(std::optional<std::string>(tier)));
    }
    void save_open_ai_responses_api_enabled(bool enabled) override {
        append("saveOpenAiResponsesApiEnabled", boolean_value(enabled));
    }
    void save_anthropic_cache_enabled(bool enabled) override {
        append("saveAnthropicCacheEnabled", boolean_value(enabled));
    }
    void save_anthropic_cache_ttl(const std::string& ttl) override {
        append("saveAnthropicCacheTtl", string_value(std::optional<std::string>(ttl)));
    }
    void save_provider_base_urls(const archive::StringMap& urls) override {
        append("saveProviderBaseUrls", map_value(urls));
    }
    void save_title_generation_enabled(bool enabled) override {
        append("saveTitleGenerationEnabled", boolean_value(enabled));
    }
    void save_title_generation_model(const std::optional<std::string>& model) override {
        append("saveTitleGenerationModel", string_value(model));
    }
    void save_title_generation_prompt(const std::string& prompt) override {
        append("saveTitleGenerationPrompt", string_value(std::optional<std::string>(prompt)));
    }
    void save_title_generation_notifications_enabled(bool enabled) override {
        append("saveTitleGenerationNotificationsEnabled", boolean_value(enabled));
    }
    void save_access_past_conversations(bool enabled) override {
        append("saveAccessPastConversations", boolean_value(enabled));
    }
    void save_access_saved_memories(bool enabled) override {
        append("saveAccessSavedMemories", boolean_value(enabled));
    }
    void save_access_active_memory(bool enabled) override {
        append("saveAccessActiveMemory", boolean_value(enabled));
    }
    void save_access_skills(bool enabled) override { append("saveAccessSkills", boolean_value(enabled)); }
    void save_rag_search_enabled(bool enabled) override {
        append("saveRagSearchEnabled", boolean_value(enabled));
    }
    void save_model_search_method(const std::string& method) override {
        append("saveModelSearchMethod", string_value(std::optional<std::string>(method)));
    }
    void save_manual_search_method(const std::string& method) override {
        append("saveManualSearchMethod", string_value(std::optional<std::string>(method)));
    }
    void save_embedding_models(const std::vector<backup::EmbeddingModelConfig>& models) override {
        append("saveEmbeddingModels", embedding_value(models));
    }
    void set_active_embedding_model_id(const std::string& id) override {
        append("setActiveEmbeddingModelId", string_value(std::optional<std::string>(id)));
    }
    void save_app_language(const std::string& language) override {
        append("saveAppLanguage", string_value(std::optional<std::string>(language)));
    }
    void save_web_search_enabled(bool enabled) override {
        append("saveWebSearchEnabled", boolean_value(enabled));
    }
    void save_web_search_provider(const std::string& provider) override {
        append("saveWebSearchProvider", string_value(std::optional<std::string>(provider)));
    }
    void save_web_search_num_results(int count) override {
        append("saveWebSearchNumResults", int_value(count));
    }
    void save_web_search_base_url(const std::string& url) override {
        append("saveWebSearchBaseUrl", string_value(std::optional<std::string>(url)));
    }
    void save_image_gen_enabled(bool enabled) override {
        append("saveImageGenEnabled", boolean_value(enabled));
    }
    void save_image_gen_model(const std::optional<std::string>& model) override {
        append("saveImageGenModel", string_value(model));
    }
    void save_image_gen_size(const std::string& size) override {
        append("saveImageGenSize", string_value(std::optional<std::string>(size)));
    }
    void save_search_context_window(int window) override {
        append("saveSearchContextWindow", int_value(window));
    }
    void save_search_match_limit(int limit) override {
        append("saveSearchMatchLimit", int_value(limit));
    }
    void save_rag_threshold(float threshold) override {
        append("saveRagThreshold", float_value(threshold));
    }
    void save_auto_cache_enabled(bool enabled) override {
        append("saveAutoCacheEnabled", boolean_value(enabled));
    }
    void save_show_uncached_notification(bool enabled) override {
        append("saveShowUncachedNotification", boolean_value(enabled));
    }
    void save_auto_update_check(bool enabled) override {
        append("saveAutoUpdateCheck", boolean_value(enabled));
    }
    void save_image_transcription_enabled(bool enabled) override {
        append("saveImageTranscriptionEnabled", boolean_value(enabled));
    }
    void save_image_transcription_enabled_models(
        const std::vector<std::string>& models) override {
        append("saveImageTranscriptionEnabledModels", strings_value(models));
    }
    void save_image_transcription_model(const std::optional<std::string>& model) override {
        append("saveImageTranscriptionModel", string_value(model));
    }
    void save_image_transcription_batch_size(int size) override {
        append("saveImageTranscriptionBatchSize", int_value(size));
    }
    void save_image_transcription_prompt(const std::string& prompt) override {
        append("saveImageTranscriptionPrompt", string_value(std::optional<std::string>(prompt)));
    }
    void save_shell_enabled(bool enabled) override { append("saveShellEnabled", boolean_value(enabled)); }
    void save_shell_confirm_enabled(bool enabled) override {
        append("saveShellConfirmEnabled", boolean_value(enabled));
    }
    void save_ask_user_enabled(bool enabled) override {
        append("saveAskUserEnabled", boolean_value(enabled));
    }
    void save_shell_devices(const std::vector<backup::ShellDeviceConfig>& devices) override {
        append("saveShellDevices", shell_value(devices));
    }
    void save_automation_tools_enabled(bool enabled) override {
        append("saveAutomationToolsEnabled", boolean_value(enabled));
    }
    void save_exact_execution_enabled(bool enabled) override {
        append("saveExactExecutionEnabled", boolean_value(enabled));
    }
    void save_automation_wake_lock_enabled(bool enabled) override {
        append("saveAutomationWakeLockEnabled", boolean_value(enabled));
    }
    void save_custom_providers(const std::vector<archive::CustomProviderConfig>& providers) override {
        append("saveCustomProviders", provider_value(providers));
    }
    void save_mcp_servers(const std::vector<backup::McpServerConfig>& servers) override {
        append("saveMcpServers", mcp_value(servers));
    }
    void save_proxy_enabled(bool enabled) override { append("saveProxyEnabled", boolean_value(enabled)); }
    void save_proxy_type(const std::string& type) override {
        append("saveProxyType", string_value(std::optional<std::string>(type)));
    }
    void save_proxy_host(const std::string& host) override {
        append("saveProxyHost", string_value(std::optional<std::string>(host)));
    }
    void save_proxy_port(const std::string& port) override {
        append("saveProxyPort", string_value(std::optional<std::string>(port)));
    }
    void save_proxy_username(const std::string& username) override {
        append("saveProxyUsername", string_value(std::optional<std::string>(username)));
    }
    void save_proxy_bypass(const std::string& bypass) override {
        append("saveProxyBypass", string_value(std::optional<std::string>(bypass)));
    }
    void save_show_documentation_fab(bool enabled) override {
        append("saveShowDocumentationFab", boolean_value(enabled));
    }
    void save_theme_mode(const std::string& mode) override {
        append("saveThemeMode", string_value(std::optional<std::string>(mode)));
    }
    void save_amoled_enabled(bool enabled) override { append("saveAmoledEnabled", boolean_value(enabled)); }
    void save_color_scheme(const std::string& scheme) override {
        append("saveColorScheme", string_value(std::optional<std::string>(scheme)));
    }
    void save_dynamic_color(bool enabled) override { append("saveDynamicColor", boolean_value(enabled)); }
    void save_blur_effects_enabled(bool enabled) override {
        append("saveBlurEffectsEnabled", boolean_value(enabled));
    }
    void save_reduce_motion(bool enabled) override { append("saveReduceMotion", boolean_value(enabled)); }
    void save_stick_to_bottom(bool enabled) override {
        append("saveStickToBottom", boolean_value(enabled));
    }
    void save_parse_inline_dollar_math(bool enabled) override {
        append("saveParseInlineDollarMath", boolean_value(enabled));
    }
    void save_auto_wrap_code_blocks(bool enabled) override {
        append("saveAutoWrapCodeBlocks", boolean_value(enabled));
    }
    void save_haptics_enabled(bool enabled) override { append("saveHapticsEnabled", boolean_value(enabled)); }
    void save_detailed_token_usage(bool enabled) override {
        append("saveDetailedTokenUsage", boolean_value(enabled));
    }
    void save_tool_call_display_mode(const std::string& mode) override {
        append("saveToolCallDisplayMode", string_value(std::optional<std::string>(mode)));
    }
    void save_thinking_segment_display_mode(const std::string& mode) override {
        append("saveThinkingSegmentDisplayMode", string_value(std::optional<std::string>(mode)));
    }
    void save_auto_expand_active_group(bool enabled) override {
        append("saveAutoExpandActiveGroup", boolean_value(enabled));
    }
    void save_scheme_style(const std::string& style) override {
        append("saveSchemeStyle", string_value(std::optional<std::string>(style)));
    }
    void save_font_preference(const std::string& preference) override {
        append("saveFontPreference", string_value(std::optional<std::string>(preference)));
    }
    void save_custom_font_path(const std::string& path) override {
        append("saveCustomFontPath", string_value(std::optional<std::string>(path)));
    }
    void save_custom_font_name(const std::string& name) override {
        append("saveCustomFontName", string_value(std::optional<std::string>(name)));
    }
    void save_default_temperature(const std::optional<float>& value) override {
        append("saveDefaultTemperature", float_value(value));
    }
    void save_default_max_tokens(const std::optional<int>& value) override {
        if (value.has_value()) {
            append("saveDefaultMaxTokens", int_value(*value));
        } else {
            append("saveDefaultMaxTokens", "{\"int\":null}");
        }
    }
    void save_default_top_p(const std::optional<float>& value) override {
        append("saveDefaultTopP", float_value(value));
    }
    void save_default_frequency_penalty(const std::optional<float>& value) override {
        append("saveDefaultFrequencyPenalty", float_value(value));
    }
    void save_default_presence_penalty(const std::optional<float>& value) override {
        append("saveDefaultPresencePenalty", float_value(value));
    }
    void set_active_system_prompt_id(const std::optional<std::string>& id) override {
        append("setActiveSystemPromptId", string_value(id));
    }
    void reset_portable_settings_for_import() override {
        append("resetPortableSettingsForImport", "{\"none\":true}");
    }
    void invalidate_portable_model_caches() override {
        append("invalidatePortableModelCaches", "{\"none\":true}");
    }
};

// ---------------------------------------------------------------------------------- trace render

std::string stable_id_mask(const std::string& text, const std::unordered_set<std::string>& known) {
    static const std::regex pattern(
        "custom-provider-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-"
        "[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}");
    std::string out;
    auto begin = std::sregex_iterator(text.begin(), text.end(), pattern);
    const auto end = std::sregex_iterator();
    std::size_t copied = 0;
    for (auto it = begin; it != end; ++it) {
        const std::smatch match = *it;
        out.append(text, copied, static_cast<std::size_t>(match.position()) - copied);
        copied = static_cast<std::size_t>(match.position() + match.length());
        std::string lowered = match.str();
        for (char& c : lowered) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        if (known.count(lowered) != 0) {
            out += match.str();
        } else {
            out += "custom-provider-<generated>";
        }
    }
    out.append(text, copied, std::string::npos);
    return out;
}

std::unordered_set<std::string> known_ids_in(const std::string& text) {
    static const std::regex pattern(
        "custom-provider-[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[1-5][0-9a-fA-F]{3}-"
        "[89abAB][0-9a-fA-F]{3}-[0-9a-fA-F]{12}");
    std::unordered_set<std::string> known;
    for (auto it = std::sregex_iterator(text.begin(), text.end(), pattern); it != std::sregex_iterator();
         ++it) {
        std::string lowered = it->str();
        for (char& c : lowered) {
            if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
        }
        known.insert(lowered);
    }
    return known;
}

bool scenario_flag(const Value& root, std::string_view name, bool fallback) {
    const Value* value = field(root, name);
    if (value == nullptr) return fallback;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a bool");
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return fallback;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string raw = read_file(path);
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const Value* op = field(root, "op");
    if (op == nullptr || op->kind != Kind::String) scenario_fail("`op` must be a string");
    const archive::SettingsSnapshot snapshot = read_snapshot(root);
    const std::unordered_set<std::string> known = known_ids_in(raw);

    if (op->text == "export") {
        const bool custom_font_included = scenario_flag(root, "customFontIncluded", false);
        const std::string rendered = json::to_kotlin_string(
            archive::to_json_object(snapshot, custom_font_included));
        return stable_id_mask(rendered, known);
    }
    if (op->text != "restore") scenario_fail(path.stem().string() + ": unknown op");

    const Value* object = field(root, "object");
    if (object == nullptr || object->kind != Kind::Object) {
        scenario_fail("`object` must be an object");
    }
    archive::RestoreOptions options;
    options.replace = scenario_flag(root, "replace", false);
    options.allow_legacy_secrets = scenario_flag(root, "allowLegacySecrets", false);
    if (const Value* font = field(root, "restoredCustomFont"); font != nullptr) {
        if (font->kind != Kind::Literal || font->text != "null") {
            if (font->kind != Kind::Object) scenario_fail("restoredCustomFont must be an object");
            archive::CustomFont custom_font;
            if (const Value* path_value = field(*font, "path"); path_value != nullptr) {
                custom_font.path = path_value->text;
            }
            if (const Value* display = field(*font, "displayName"); display != nullptr) {
                custom_font.display_name = display->text;
            }
            options.restored_custom_font = custom_font;
        }
    }
    std::unordered_map<std::string, std::string> prompt_map;
    if (const Value* prompts = field(root, "resolveSystemPromptIds"); prompts != nullptr) {
        if (prompts->kind == Kind::Object) {
            for (const Member& member : prompts->members) {
                if (member.value.kind == Kind::String) {
                    prompt_map[member.key] = member.value.text;
                }
            }
        }
    }
    options.resolve_system_prompt_id = [&](const std::optional<std::string>& id) {
        if (!id.has_value()) return std::optional<std::string>{};
        const auto it = prompt_map.find(*id);
        if (it == prompt_map.end()) return std::optional<std::string>{};
        return std::optional<std::string>(it->second);
    };
    int minted = 0;
    options.provider_id_generator = [&minted]() {
        char buffer[64];
        std::snprintf(buffer, sizeof(buffer), "custom-provider-00000000-0000-4000-8000-%012d",
                      ++minted);
        return std::string(buffer);
    };

    const bool font_file_exists = scenario_flag(root, "fontFileExists", false);
    const std::filesystem::path platform_font("platform_font.ttf");
    if (font_file_exists) {
        std::ofstream handle(platform_font, std::ios::binary);
        handle << "platform font";
    } else {
        std::error_code ignored;
        std::filesystem::remove(platform_font, ignored);
    }

    RecordingStore store;
    std::string error_type;
    std::string error_message;
    archive::RestoreResult restore_result;
    try {
        restore_result = archive::restore_from_json_object(*object, snapshot, store, options);
    } catch (const archive::IllegalArgumentException& error) {
        error_type = error.kotlin_type();
        error_message = error.what();
    }

    std::string trace = "{\"calls\":[" + store.calls + "],\"warnings\":[";
    for (std::size_t i = 0; i < restore_result.warnings.size(); ++i) {
        if (i != 0) trace.push_back(',');
        append_quoted(trace, restore_result.warnings[i]);
    }
    trace.push_back(']');
    if (!error_type.empty()) {
        trace += ",\"error\":{\"type\":";
        append_quoted(trace, error_type);
        trace += ",\"message\":";
        append_quoted(trace, error_message);
        trace.push_back('}');
    }
    trace.push_back('}');
    return stable_id_mask(trace, known);
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

TEST(portable_settings_archive_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PORTABLE_SETTINGS_ARCHIVE_DIR;
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
    std::error_code ignored;
    std::filesystem::remove("platform_font.ttf", ignored);
    CHECK(checked == static_cast<int>(scenarios.size()));
}
