// Golden parity tests for data/DataExporter.kt.
//
// The Kotlin stand (core/tools/golden/data-exporter) compiles the app's DataExporter.kt and every
// module it calls verbatim over a real SQLite database and Apache Commons Compress, freezing one
// trace per scenario: the progress floats, the ExportResult or thrown error, and the archive's
// entry names/contents (media entry UUIDs masked by first appearance, exported_at masked as
// <clock>, conv/index.json mediaEntries re-sorted by ordinal because the sort key is UUID
// entropy). This test replays every scenario through the C++ port over the same schema and
// requires the emitted trace to match byte for byte at the JSON value level.
//
// The settings-snapshot readers below are the exact block the portable-settings-archive parity
// test uses (scenario -> SettingsSnapshot), copied so the two tests stay independently
// compilable; both are checked against their own frozen traces.

#include <algorithm>
#include <cerrno>
#include <cstdint>
#include <cstdio>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <limits>
#include <map>
#include <memory>
#include <optional>
#include <regex>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <unistd.h>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "agora/core/chat_dao.hpp"
#include "agora/core/conversation_export_snapshot_reader.hpp"
#include "agora/core/data_exporter.hpp"
#include "agora/core/export_extra_settings.hpp"
#include "agora/core/export_spool.hpp"
#include "agora/core/json.hpp"
#include "agora/core/memory_manager.hpp"
#include "agora/core/native_backup_format.hpp"
#include "agora/core/native_backup_secrets.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/portable_settings_archive.hpp"
#include "agora/core/skill_manager.hpp"
#include "agora/core/sql_boundary.hpp"
#include "agora/core/zip_reader.hpp"
#include "agora/core/zip_writer.hpp"
#include "check.hpp"

namespace {

namespace archive = agora::core::settings_archive;
namespace json = agora::core::json;
namespace backup = agora::core::native_backup;
namespace data_export = agora::core::data_export;
namespace export_extra_settings = agora::core::export_extra_settings;
namespace export_snapshot = agora::core::conversation_export_snapshot;
namespace export_spool = agora::core::export_spool;
namespace native_backup = agora::core::native_backup;
namespace zip = agora::core::zip;

using agora::core::KotlinError;
using agora::core::SqlArgs;
using agora::core::SqlExecutor;
using agora::core::SqlRow;
using agora::core::SqlValue;
using json::Kind;
using json::Member;
using json::Value;

// ---- copied from portable_settings_archive_parity_test.cpp (scenario -> SettingsSnapshot)
// ---- so the two tests stay independently compilable -------------------------------
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
// ---- end copied settings readers -----------------------------------------------


// ---------------------------------------------------------------------------------------------
// Trace building: JSON values, masking and archive rendering
// ---------------------------------------------------------------------------------------------

json::Value json_string(std::string_view text) {
    Value v;
    v.kind = Kind::String;
    v.text = std::string(text);
    return v;
}

json::Value json_long(std::int64_t number) {
    Value v;
    v.kind = Kind::Literal;
    v.text = std::to_string(number);
    return v;
}

json::Value json_array(std::vector<json::Value> elements) {
    Value v;
    v.kind = Kind::Array;
    v.elements = std::move(elements);
    return v;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    Value v;
    v.kind = Kind::Object;
    for (auto& [key, value] : members) v.members.push_back({std::move(key), std::move(value)});
    return v;
}

bool json_equal(const json::Value& left, const json::Value& right) {
    if (left.kind != right.kind) return false;
    if (left.kind == Kind::String || left.kind == Kind::Literal) {
        return left.text == right.text;
    }
    if (left.kind == Kind::Array) {
        if (left.elements.size() != right.elements.size()) return false;
        for (std::size_t i = 0; i < left.elements.size(); ++i) {
            if (!json_equal(left.elements[i], right.elements[i])) return false;
        }
        return true;
    }
    if (left.members.size() != right.members.size()) return false;
    for (const json::Member& member : left.members) {
        const json::Value* match = field(right, member.key);
        if (match == nullptr || !json_equal(member.value, *match)) return false;
    }
    return true;
}

std::string excerpt(const json::Value& value, std::size_t limit = 400) {
    std::string text = json::to_kotlin_string(value);
    if (text.size() > limit) return text.substr(0, limit) + "...";
    return text;
}

// ---- base64 (the fixtures and the trace mirror java.util.Base64) -----------------------------

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::string& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = static_cast<unsigned char>(bytes[i]);
        const std::uint32_t b = i + 1 < bytes.size() ? static_cast<unsigned char>(bytes[i + 1]) : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? static_cast<unsigned char>(bytes[i + 2]) : 0;
        const std::uint32_t value = (a << 16) | (b << 8) | c;
        out.push_back(kBase64[(value >> 18) & 63]);
        out.push_back(kBase64[(value >> 12) & 63]);
        out.push_back(i + 1 < bytes.size() ? kBase64[(value >> 6) & 63] : '=');
        out.push_back(i + 2 < bytes.size() ? kBase64[value & 63] : '=');
    }
    return out;
}

std::vector<std::uint8_t> base64_decode(std::string_view text) {
    std::vector<std::uint8_t> out;
    int accumulator = 0;
    int bits = 0;
    for (const char ch : text) {
        if (ch == '=' || ch == '\n' || ch == '\r') break;
        int value = -1;
        if (ch >= 'A' && ch <= 'Z') value = ch - 'A';
        else if (ch >= 'a' && ch <= 'z') value = ch - 'a' + 26;
        else if (ch >= '0' && ch <= '9') value = ch - '0' + 52;
        else if (ch == '+') value = 62;
        else if (ch == '/') value = 63;
        if (value < 0) continue;
        accumulator = (accumulator << 6) | value;
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- step fields ------------------------------------------------------------------------------

/**
 * Fallback UUID for a scenario field Kotlin would generate randomly. Every frozen scenario
 * supplies these ids explicitly, so this deterministic stand-in is never observable in a trace.
 */
std::string backup_uuid() {
    static int counter = 0;
    char buffer[37];
    std::snprintf(buffer, sizeof(buffer), "%08x-0000-4000-8000-%012x", counter, counter);
    ++counter;
    return std::string(buffer);
}

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

bool optional_flag(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (value == nullptr) return fallback;
    if (value->kind == Kind::Literal && value->text == "true") return true;
    if (value->kind == Kind::Literal && value->text == "false") return false;
    scenario_fail(std::string(name) + " must be a bool");
}

std::optional<int> optional_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be an int");
    const std::optional<int> parsed = parse_int_for_scenario(value->text);
    if (!parsed.has_value()) scenario_fail(std::string(name) + " is not an int");
    return parsed;
}

std::vector<std::string> string_array(const Value* value) {
    if (value == nullptr) return {};
    if (value->kind != Kind::Array) scenario_fail("expected a string array");
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail("expected a string element");
        out.push_back(element.text);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// ConversationSettings / SystemPromptEntry scenario readers
// ---------------------------------------------------------------------------------------------

std::optional<int> opt_int(const Value& object, std::string_view name) {
    return field(object, name) == nullptr ? std::nullopt : read_nullable_int(object, name);
}

std::optional<float> opt_float(const Value& object, std::string_view name) {
    return field(object, name) == nullptr ? std::nullopt : read_nullable_float(object, name);
}

std::optional<bool> opt_bool(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    return read_bool(object, name);
}

std::optional<std::string> opt_text(const Value& object, std::string_view name) {
    return field(object, name) == nullptr ? std::nullopt : read_nullable_string(object, name);
}

/** data/SettingsContracts.kt::ConversationSettings — every field nullable (default null). */
export_extra_settings::ConversationSettings read_conversation_settings(const Value& object) {
    export_extra_settings::ConversationSettings settings;
    settings.context_window = opt_int(object, "contextWindow");
    settings.temperature = opt_float(object, "temperature");
    settings.max_tokens = opt_int(object, "maxTokens");
    settings.top_p = opt_float(object, "topP");
    settings.frequency_penalty = opt_float(object, "frequencyPenalty");
    settings.presence_penalty = opt_float(object, "presencePenalty");
    settings.code_execution_enabled = opt_bool(object, "codeExecutionEnabled");
    settings.google_search_enabled = opt_bool(object, "googleSearchEnabled");
    settings.open_ai_web_search_enabled = opt_bool(object, "openAiWebSearchEnabled");
    settings.thinking_enabled = opt_bool(object, "thinkingEnabled");
    settings.thinking_level = opt_text(object, "thinkingLevel");
    settings.thinking_budget_enabled = opt_bool(object, "thinkingBudgetEnabled");
    settings.thinking_budget_tokens = opt_int(object, "thinkingBudgetTokens");
    settings.open_ai_service_tier_enabled = opt_bool(object, "openAiServiceTierEnabled");
    settings.open_ai_service_tier = opt_text(object, "openAiServiceTier");
    settings.web_search_enabled = opt_bool(object, "webSearchEnabled");
    settings.shell_enabled = opt_bool(object, "shellEnabled");
    settings.low_context_mode_enabled = opt_bool(object, "lowContextModeEnabled");
    return settings;
}

std::map<std::string, export_extra_settings::ConversationSettings> read_conversation_settings_map(
    const Value& scenario) {
    std::map<std::string, export_extra_settings::ConversationSettings> out;
    const Value* mapping = field(scenario, "conversationSettings");
    if (mapping == nullptr) return out;
    if (mapping->kind != Kind::Object) scenario_fail("conversationSettings must be an object");
    for (const Member& member : mapping->members) {
        if (member.value.kind != Kind::Object) {
            scenario_fail("conversationSettings values must be objects");
        }
        out[member.key] = read_conversation_settings(member.value);
    }
    return out;
}

std::vector<data_export::PromptTemplateItem> read_prompt_items(const Value& object,
                                                               std::string_view name) {
    std::vector<data_export::PromptTemplateItem> out;
    const Value* value = field(object, name);
    if (value == nullptr) return out;  // default emptyList
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail(std::string(name) + " entries need objects");
        data_export::PromptTemplateItem item;
        item.id = field(element, "id") != nullptr ? read_string(element, "id")
                                                  : backup_uuid();
        item.type = required_string(element, "type");
        item.value = required_string(element, "value");
        out.push_back(std::move(item));
    }
    return out;
}

std::vector<data_export::SystemPromptEntry> read_system_prompts(const Value& scenario) {
    std::vector<data_export::SystemPromptEntry> out;
    const Value* value = field(scenario, "systemPrompts");
    if (value == nullptr) return out;
    if (value->kind != Kind::Array) scenario_fail("systemPrompts must be an array");
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail("systemPrompts entries need objects");
        data_export::SystemPromptEntry entry;
        entry.id = required_string(element, "id");
        entry.title = required_string(element, "title");
        entry.content = field(element, "content") != nullptr
                            ? read_string(element, "content")
                            : std::string();
        entry.system_items = read_prompt_items(element, "systemItems");
        entry.user_items = read_prompt_items(element, "userItems");
        entry.assistant_items = read_prompt_items(element, "assistantItems");
        entry.user_prepend_items = read_prompt_items(element, "userPrependItems");
        entry.user_postpend_items = read_prompt_items(element, "userPostpendItems");
        out.push_back(std::move(entry));
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// Secrets store (the SettingsManager read surface NativeBackupSecretsPolicy.capture drives)
// ---------------------------------------------------------------------------------------------

class ScenarioSecrets final : public backup::SettingsStore {
public:
    // Set by the runner before the export runs.
    std::vector<backup::ApiKeyEntry> api_keys_storage;
    backup::StringMap active_api_key_ids_storage;
    backup::StringMap web_search_api_keys_storage;
    std::string proxy_password_storage;
    const std::vector<backup::ShellDeviceConfig>* shell_devices_ref = nullptr;
    const std::vector<backup::EmbeddingModelConfig>* embedding_models_ref = nullptr;
    const std::vector<backup::McpServerConfig>* mcp_servers_ref = nullptr;

    const std::vector<backup::ApiKeyEntry>& api_keys() const override {
        return api_keys_storage;
    }
    const backup::StringMap& active_api_key_ids() const override {
        return active_api_key_ids_storage;
    }
    const backup::StringMap& web_search_api_keys() const override {
        return web_search_api_keys_storage;
    }
    const std::string& proxy_password() const override { return proxy_password_storage; }
    const std::vector<backup::ShellDeviceConfig>& shell_devices() const override {
        return *shell_devices_ref;
    }
    const std::vector<backup::EmbeddingModelConfig>& embedding_models() const override {
        return *embedding_models_ref;
    }
    const std::vector<backup::McpServerConfig>& mcp_servers() const override {
        return *mcp_servers_ref;
    }

    void save_api_keys(const std::vector<backup::ApiKeyEntry>&) override {
        scenario_fail("capture must not save api keys");
    }
    void save_active_api_key_ids(const backup::StringMap&) override {
        scenario_fail("capture must not save active api key ids");
    }
    void save_web_search_api_keys(const backup::StringMap&) override {
        scenario_fail("capture must not save web search api keys");
    }
    void save_proxy_password(const std::string&) override {
        scenario_fail("capture must not save the proxy password");
    }
    void save_shell_devices(const std::vector<backup::ShellDeviceConfig>&) override {
        scenario_fail("capture must not save shell devices");
    }
    void save_embedding_models(const std::vector<backup::EmbeddingModelConfig>&) override {
        scenario_fail("capture must not save embedding models");
    }
    void save_mcp_servers(const std::vector<backup::McpServerConfig>&) override {
        scenario_fail("capture must not save mcp servers");
    }
};

void read_secrets_scenario(const Value& scenario, ScenarioSecrets& secrets) {
    if (const Value* keys = field(scenario, "apiKeys")) {
        if (keys->kind != Kind::Array) scenario_fail("apiKeys must be an array");
        for (const Value& element : keys->elements) {
            if (element.kind != Kind::Object) scenario_fail("apiKeys entries need objects");
            backup::ApiKeyEntry entry;
            entry.id = field(element, "id") != nullptr ? read_string(element, "id") : backup_uuid();
            entry.name = required_string(element, "name");
            entry.key = required_string(element, "key");
            entry.provider = field(element, "provider") != nullptr
                                 ? read_string(element, "provider")
                                 : std::string("Google");
            secrets.api_keys_storage.push_back(std::move(entry));
        }
    }
    if (const Value* ids = field(scenario, "activeApiKeyIds")) {
        secrets.active_api_key_ids_storage = read_string_map(
            json_object({{"value", *ids}}), "value");
    }
    if (const Value* keys = field(scenario, "webSearchApiKeys")) {
        secrets.web_search_api_keys_storage = read_string_map(
            json_object({{"value", *keys}}), "value");
    }
    secrets.proxy_password_storage =
        field(scenario, "proxyPassword") != nullptr ? read_string(scenario, "proxyPassword") : "";
}

// ---------------------------------------------------------------------------------------------
// SQLite executor + snapshot store (the pinned ChatDao over the stand's schema)
// ---------------------------------------------------------------------------------------------

class SqliteExecutor final : public SqlExecutor {
public:
    SqliteExecutor(const std::filesystem::path& schema_file, const Value& scenario,
                   std::string_view substitution_root) {
        if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
            scenario_fail(std::string("cannot open sqlite: ") + sqlite3_errmsg(db_));
        }
        exec_raw("PRAGMA foreign_keys = ON");
        exec_raw(read_file(schema_file));
        const Value* seeds = field(scenario, "seedSql");
        if (seeds != nullptr) {
            if (seeds->kind != Kind::Array) scenario_fail("seedSql must be an array");
            for (const Value& seed : seeds->elements) {
                if (seed.kind != Kind::String) scenario_fail("seedSql entries must be strings");
                exec_raw(seed.text);
            }
        }
        (void)substitution_root;
    }

    ~SqliteExecutor() override {
        if (db_ != nullptr) sqlite3_close(db_);
    }

    std::int64_t execute(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        try {
            bind(statement, args);
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        const int rc = sqlite3_step(statement);
        if (rc == SQLITE_DONE) {
            const std::int64_t changed = sqlite3_changes(db_);
            sqlite3_finalize(statement);
            return changed;
        }
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(statement);
        throw agora::core::SqlError("sqlite: " + message + " in " + sql);
    }

    std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        try {
            bind(statement, args);
        } catch (...) {
            sqlite3_finalize(statement);
            throw;
        }
        std::vector<SqlRow> rows;
        for (;;) {
            const int rc = sqlite3_step(statement);
            if (rc == SQLITE_ROW) {
                rows.push_back(read_row(statement));
                continue;
            }
            if (rc == SQLITE_DONE) break;
            const std::string message = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            throw agora::core::SqlError("sqlite: " + message + " in " + sql);
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::int64_t last_insert_rowid() override { return sqlite3_last_insert_rowid(db_); }

    void begin() override { exec_raw("BEGIN"); }
    void commit() override { exec_raw("COMMIT"); }
    void rollback() override { exec_raw("ROLLBACK"); }

private:
    void exec_raw(const std::string& sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "exec failed";
            sqlite3_free(error);
            scenario_fail("sqlite: " + message);
        }
    }

    sqlite3_stmt* prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            throw agora::core::SqlError(std::string("sqlite prepare: ") + sqlite3_errmsg(db_));
        }
        return statement;
    }

    void bind(sqlite3_stmt* statement, const SqlArgs& args) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            const SqlValue& value = args[i];
            switch (value.kind) {
                case SqlValue::Kind::Null:
                    sqlite3_bind_null(statement, index);
                    break;
                case SqlValue::Kind::Text:
                    sqlite3_bind_text(statement, index, value.text.data(),
                                      static_cast<int>(value.text.size()), SQLITE_TRANSIENT);
                    break;
                case SqlValue::Kind::Integer:
                    sqlite3_bind_int64(statement, index, value.integer);
                    break;
                case SqlValue::Kind::Real:
                    sqlite3_bind_double(statement, index, value.real);
                    break;
                case SqlValue::Kind::Blob: {
                    const void* data = value.blob.empty()
                                           ? static_cast<const void*>("")
                                           : static_cast<const void*>(value.blob.data());
                    sqlite3_bind_blob(statement, index, data,
                                      static_cast<int>(value.blob.size()), SQLITE_TRANSIENT);
                    break;
                }
            }
        }
    }

    SqlRow read_row(sqlite3_stmt* statement) {
        const int columns = sqlite3_column_count(statement);
        SqlRow row;
        for (int i = 0; i < columns; ++i) {
            SqlValue cell;
            switch (sqlite3_column_type(statement, i)) {
                case SQLITE_NULL:
                    break;
                case SQLITE_INTEGER:
                    cell.kind = SqlValue::Kind::Integer;
                    cell.integer = sqlite3_column_int64(statement, i);
                    break;
                case SQLITE_FLOAT:
                    cell.kind = SqlValue::Kind::Real;
                    cell.real = sqlite3_column_double(statement, i);
                    break;
                case SQLITE_TEXT: {
                    cell.kind = SqlValue::Kind::Text;
                    const auto* bytes = static_cast<const unsigned char*>(
                        sqlite3_column_text(statement, i));
                    const int size = sqlite3_column_bytes(statement, i);
                    cell.text.assign(reinterpret_cast<const char*>(bytes),
                                     static_cast<std::size_t>(size));
                    break;
                }
                case SQLITE_BLOB: {
                    cell.kind = SqlValue::Kind::Blob;
                    const auto* bytes = static_cast<const unsigned char*>(
                        sqlite3_column_blob(statement, i));
                    const int size = sqlite3_column_bytes(statement, i);
                    cell.blob.assign(bytes, bytes + size);
                    break;
                }
            }
            row.push_back(std::move(cell));
        }
        return row;
    }

    sqlite3* db_ = nullptr;
};

// ---------------------------------------------------------------------------------------------
// Platform implementation
// ---------------------------------------------------------------------------------------------

class TestPlatform final : public data_export::Platform {
public:
    TestPlatform() {
        app_version = [this] { return app_version_; };
        exported_at = [this] { return exported_at_; };
        create_spool_file = [this] {
            const std::filesystem::path path =
                cache_dir_ / ("agora-export-" + std::to_string(spool_counter_++) + ".jsonl");
            std::ofstream(path).close();  // Kotlin File.createTempFile creates it empty
            return path.string();
        };
        new_uuid = [this] {
            char buffer[37];
            std::snprintf(buffer, sizeof(buffer), "%08x-0000-4000-8000-%012x", uuid_counter_,
                          uuid_counter_);
            ++uuid_counter_;
            return std::string(buffer);
        };
        open_output = [this]() -> std::unique_ptr<OutputSink> {
            if (current_output_.empty()) return nullptr;
            return std::make_unique<FileSink>(current_output_);
        };
        ensure_active = [] {};
        file_exists = [](std::string_view path) {
            std::error_code ec;
            return std::filesystem::exists(std::string(path), ec);
        };
        canonical_path = [](std::string_view path) {
            return std::filesystem::canonical(std::string(path)).string();
        };
        absolute_path = [](std::string_view path) {
            return std::filesystem::absolute(std::string(path)).string();
        };
        open_media = [this](std::string_view source) -> std::optional<std::string> {
            const std::string raw(source);
            if (const auto registered = inputs_.find(raw); registered != inputs_.end()) {
                return read_file_or_null(registered->second);
            }
            if (raw.rfind("file://", 0) == 0) {
                return read_file_or_null(raw.substr(7));
            }
            if (raw.rfind("content://", 0) == 0) {
                return std::nullopt;  // unregistered content URI: resolver returns null
            }
            return read_file_or_null(raw);
        };
    }

    void configure(std::filesystem::path cache_dir, std::string app_version_value,
                   std::string exported_at_value) {
        cache_dir_ = std::move(cache_dir);
        app_version_ = std::move(app_version_value);
        exported_at_ = std::move(exported_at_value);
    }

    /** content:// (and any registered) URI -> fixture file, like the stand's ContentResolver. */
    void register_input(std::string uri, std::filesystem::path file) {
        inputs_[std::move(uri)] = file.string();
    }

    /** nullopt makes open_output fail — the stand's unregistered destination URI. */
    void set_output(std::optional<std::filesystem::path> file) {
        current_output_ = file.has_value() ? file->string() : std::string();
    }

private:
    struct FileSink : public OutputSink {
        explicit FileSink(const std::string& path) {
            stream.open(path, std::ios::binary | std::ios::trunc);
        }
        void write(std::string_view bytes) override { stream.write(bytes.data(), bytes.size()); }
        std::ofstream stream;
    };

    static std::optional<std::string> read_file_or_null(const std::string& path) {
        std::ifstream stream(path, std::ios::binary);
        if (!stream) return std::nullopt;
        return std::string((std::istreambuf_iterator<char>(stream)),
                           std::istreambuf_iterator<char>());
    }

    std::filesystem::path cache_dir_;
    std::string app_version_ = "unknown";
    std::string exported_at_ = "2026-01-01T00:00:00Z";
    std::map<std::string, std::string> inputs_;
    std::string current_output_;
    int spool_counter_ = 0;
    int uuid_counter_ = 0;
};

// ---------------------------------------------------------------------------------------------
// Archive rendering: entries -> trace values with UUID / clock masking
// ---------------------------------------------------------------------------------------------

const std::regex& uuid_regex() {
    static const std::regex pattern(
        "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    return pattern;
}

const std::regex& exported_at_regex() {
    static const std::regex pattern("\"exported_at\":\"[^\"]*\"");
    return pattern;
}

/**
 * Renders one exported archive into the trace's `archive` array: masks every UUID that appears
 * in a `media/` entry name by first-appearance ordinal, rewrites exported_at as <clock>, re-sorts
 * conv/index.json's mediaEntries by the masked ordinal (the sort key is UUID entropy), and emits
 * media/custom_font payloads as base64 (java.util.Base64) with everything else as UTF-8 text.
 */
json::Value render_archive(const std::vector<zip::Entry>& entries) {
    std::unordered_set<std::string> generated;
    for (const zip::Entry& entry : entries) {
        if (entry.name.rfind("media/", 0) == 0) {
            if (std::smatch match; std::regex_search(entry.name, match, uuid_regex())) {
                generated.insert(match[0].str());
            }
        }
    }

    std::unordered_map<std::string, int> ordinals;
    auto mask = [&](const std::string& text) {
        std::string out;
        std::sregex_iterator it(text.begin(), text.end(), uuid_regex());
        const std::sregex_iterator end;
        std::size_t copied = 0;
        for (; it != end; ++it) {
            const std::smatch& match = *it;
            const std::string uuid = match[0].str();
            if (generated.find(uuid) == generated.end()) continue;
            out.append(text, copied, static_cast<std::size_t>(match.position(0)) - copied);
            copied = static_cast<std::size_t>(match.position(0) + match.length(0));
            const auto [entry, inserted] =
                ordinals.emplace(uuid, static_cast<int>(ordinals.size()));
            out += "<uuid:" + std::to_string(entry->second) + ">";
        }
        out.append(text, copied, std::string::npos);
        return out;
    };

    auto canonicalize_index = [&](const std::string& text) -> std::string {
        const json::ParseResult parsed = json::parse(text);
        if (!parsed.value || parsed.value->kind != Kind::Object) return text;
        const std::regex ordinal_pattern("<uuid:([0-9]+)>");
        std::vector<const Member*> conversations;
        const Value* list = field(*parsed.value, "conversations");
        if (list == nullptr || list->kind != Kind::Array) return text;
        struct Entry {
            const Value* item;
            int key;
        };
        std::vector<Entry> items;
        for (const Value& item : list->elements) {
            const Value* media = field(item, "mediaEntries");
            int key = 0;
            if (media != nullptr && media->kind == Kind::Array && !media->elements.empty()) {
                std::smatch match;
                const std::string& first = media->elements.front().text;
                if (std::regex_search(first, match, ordinal_pattern)) {
                    key = std::stoi(match[1].str());
                }
            }
            items.push_back({&item, key});
        }
        std::stable_sort(items.begin(), items.end(),
                         [](const Entry& a, const Entry& b) { return a.key < b.key; });
        std::vector<json::Value> rebuilt;
        for (const Entry& entry : items) {
            Value item = *entry.item;
            const Value* media = field(item, "mediaEntries");
            if (media != nullptr && media->kind == Kind::Array) {
                std::vector<Value> sorted = media->elements;
                std::stable_sort(sorted.begin(), sorted.end(),
                                 [](const Value& a, const Value& b) {
                                     const std::regex ordinal_pattern2("<uuid:([0-9]+)>");
                                     std::smatch ma;
                                     std::smatch mb;
                                     const int ka = std::regex_search(a.text, ma, ordinal_pattern2)
                                                        ? std::stoi(ma[1].str())
                                                        : 0;
                                     const int kb = std::regex_search(b.text, mb, ordinal_pattern2)
                                                        ? std::stoi(mb[1].str())
                                                        : 0;
                                     return ka < kb;
                                 });
                for (Member& member : item.members) {
                    if (member.key == "mediaEntries") {
                        Value replaced;
                        replaced.kind = Kind::Array;
                        replaced.elements = sorted;
                        member.value = replaced;
                    }
                }
            }
            rebuilt.push_back(item);
        }
        Value out;
        out.kind = Kind::Object;
        for (const Member& member : parsed.value->members) {
            if (member.key == "conversations") {
                Value replaced;
                replaced.kind = Kind::Array;
                replaced.elements = rebuilt;
                out.members.push_back({"conversations", replaced});
            } else {
                out.members.push_back(member);
            }
        }
        return json::to_kotlin_string(out);
    };

    std::vector<json::Value> rendered;
    for (const zip::Entry& entry : entries) {
        Value element;
        element.kind = Kind::Object;
        element.members.push_back({"name", json_string(mask(entry.name))});
        const bool binary = entry.name.rfind("media/", 0) == 0 ||
                            entry.name.rfind("custom_font/", 0) == 0;
        if (binary) {
            element.members.push_back({"base64", json_string(base64_encode(entry.content))});
        } else {
            std::string text = mask(entry.content);
            text = std::regex_replace(text, exported_at_regex(), "\"exported_at\":\"<clock>\"");
            if (entry.name == "conv/index.json") text = canonicalize_index(text);
            element.members.push_back({"text", json_string(text)});
        }
        rendered.push_back(element);
    }
    return json_array(std::move(rendered));
}

// ---------------------------------------------------------------------------------------------
// Scenario runner
// ---------------------------------------------------------------------------------------------

std::string replace_all(std::string text, std::string_view needle, std::string_view value) {
    std::size_t position = 0;
    while ((position = text.find(needle, position)) != std::string::npos) {
        text.replace(position, needle.size(), value);
        position += value.size();
    }
    return text;
}

std::size_t count_files(const std::filesystem::path& directory) {
    std::error_code ec;
    std::size_t count = 0;
    for (const auto& entry : std::filesystem::directory_iterator(directory, ec)) {
        if (entry.is_regular_file(ec)) ++count;
    }
    return count;
}

struct ExportStepOutcome {
    bool failed = false;
    std::string error_type;
    std::string error_message;
    data_export::ExportResult result;
    std::vector<std::string> progress;
};

ExportStepOutcome run_export(data_export::ExportRequest request,
                             export_snapshot::SnapshotStore& store,
                             const data_export::Inputs& inputs,
                             TestPlatform& platform) {
    ExportStepOutcome outcome;
    try {
        outcome.result = data_export::export_data(
            store, inputs, platform, request, [&](float value) {
                outcome.progress.push_back(agora::core::openai::format_kotlin_float(value));
            });
    } catch (const data_export::IOException& error) {
        outcome.failed = true;
        outcome.error_type = "IOException";
        outcome.error_message = error.what();
    } catch (const export_spool::SpoolException& error) {
        outcome.failed = true;
        outcome.error_type = error.kotlin_type();
        outcome.error_message = error.what();
    } catch (const KotlinError& error) {
        outcome.failed = true;
        outcome.error_type = error.class_name();
        outcome.error_message = error.what();
    } catch (const std::exception& error) {
        outcome.failed = true;
        outcome.error_type = "RuntimeException";
        outcome.error_message = error.what();
    }
    return outcome;
}

void stamp_baseline(const std::filesystem::path& file) {
    zip::Reader reader(file.string());
    const std::vector<zip::Entry> entries = reader.entries();
    zip::Writer writer;
    for (const zip::Entry& entry : entries) {
        if (entry.name == "manifest.json") {
            std::string text = entry.content;
            if (text.empty() || text.back() != '}') scenario_fail("baseline manifest malformed");
            text.pop_back();
            text += ",\"incremental_baseline\":1}";
            writer.put_entry(entry.name, text);
        } else {
            writer.put_entry(entry.name, entry.content);
        }
    }
    const std::string archived = writer.finish();
    std::ofstream stream(file, std::ios::binary | std::ios::trunc);
    stream.write(archived.data(), static_cast<std::streamsize>(archived.size()));
}

void run_scenario(const std::filesystem::path& scenario_path,
                  const std::filesystem::path& golden_path,
                  const std::filesystem::path& schema_path) {
    const std::string raw_text = read_file(scenario_path);
    const json::ParseResult raw_parsed = json::parse(raw_text);
    if (!raw_parsed.value || raw_parsed.value->kind != Kind::Object) {
        scenario_fail("scenario is not an object: " + scenario_path.filename().string());
    }
    const Value& raw_scenario = *raw_parsed.value;

    // Scratch tree: media/ files/ cache/ out/, mirroring the Kotlin driver's temp directory.
    static int scenario_counter = 0;
    const std::filesystem::path root = std::filesystem::temp_directory_path() /
                                       ("agora-data-export-" +
                                        std::to_string(scenario_counter++) + "-" +
                                        std::to_string(::getpid()));
    std::filesystem::remove_all(root);
    const std::filesystem::path media_root = root / "media";
    const std::filesystem::path files_dir = root / "files";
    const std::filesystem::path cache_dir = root / "cache";
    const std::filesystem::path out_dir = root / "out";
    std::filesystem::create_directories(media_root);
    std::filesystem::create_directories(files_dir / "memory_db");
    std::filesystem::create_directories(files_dir / "skill_db");
    std::filesystem::create_directories(cache_dir);
    std::filesystem::create_directories(out_dir);

    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() { std::error_code ec; std::filesystem::remove_all(path, ec); }
    } cleanup{root};

    // Font: the name comes from the unsubstituted scenario, exactly like the Kotlin driver.
    std::optional<std::filesystem::path> font_file;
    const Value* font_spec = field(raw_scenario, "font");
    if (font_spec != nullptr && font_spec->kind == Kind::Object) {
        const Value* name = field(*font_spec, "name");
        if (name != nullptr && name->kind == Kind::String) {
            font_file = files_dir / "font" / name->text;
        }
    }
    const std::string font_placeholder =
        font_file.has_value()
            ? font_file->string()
            : (files_dir / "font" / "missing").string();

    const std::string text = replace_all(
        replace_all(replace_all(raw_text, "<root>", root.string()), "<media>", media_root.string()),
        "<font>", font_placeholder);

    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail("scenario parse failed");
    const Value& scenario = *parsed.value;

    // ---- fixtures -----------------------------------------------------------------------------
    TestPlatform platform;
    platform.configure(cache_dir, required_string(scenario, "appVersion"),
                       "2026-01-01T00:00:00Z");

    if (const Value* media = field(scenario, "media")) {
        if (media->kind != Kind::Array) scenario_fail("media must be an array");
        for (const Value& element : media->elements) {
            if (element.kind != Kind::Object) scenario_fail("media entries need objects");
            const Value* file = field(element, "file");
            if (file == nullptr || file->kind != Kind::String) {
                scenario_fail("media.file must be a string");
            }
            const std::filesystem::path target = media_root / file->text;
            if (const Value* content = field(element, "base64")) {
                if (content->kind != Kind::String) scenario_fail("media.base64 must be a string");
                std::filesystem::create_directories(target.parent_path());
                const std::vector<std::uint8_t> bytes = base64_decode(content->text);
                std::ofstream stream(target, std::ios::binary | std::ios::trunc);
                stream.write(reinterpret_cast<const char*>(bytes.data()),
                             static_cast<std::streamsize>(bytes.size()));
            }
            if (const Value* uri = field(element, "uri")) {
                if (uri->kind != Kind::String) scenario_fail("media.uri must be a string");
                platform.register_input(uri->text, target);
            }
        }
    }

    if (font_spec != nullptr && font_spec->kind == Kind::Object) {
        const Value* missing = field(*font_spec, "missing");
        const bool is_missing = missing != nullptr && missing->kind == Kind::Literal &&
                                missing->text == "true";
        if (!is_missing && font_file.has_value()) {
            std::filesystem::create_directories(font_file->parent_path());
            const Value* content = field(*font_spec, "base64");
            const std::vector<std::uint8_t> bytes =
                content != nullptr && content->kind == Kind::String
                    ? base64_decode(content->text)
                    : std::vector<std::uint8_t>{};
            std::ofstream stream(*font_file, std::ios::binary | std::ios::trunc);
            stream.write(reinterpret_cast<const char*>(bytes.data()),
                         static_cast<std::streamsize>(bytes.size()));
        }
    }

    auto write_text_file = [](const std::filesystem::path& path, std::string_view content) {
        std::filesystem::create_directories(path.parent_path());
        std::ofstream stream(path, std::ios::binary | std::ios::trunc);
        stream.write(content.data(), static_cast<std::streamsize>(content.size()));
    };

    if (const Value* memory = field(scenario, "memory"); memory != nullptr && memory->kind == Kind::Object) {
        if (const Value* active = field(*memory, "active"); active != nullptr) {
            write_text_file(files_dir / "active_memory.md", active->text);
        }
        if (const Value* files = field(*memory, "files"); files != nullptr && files->kind == Kind::Object) {
            for (const Member& member : files->members) {
                write_text_file(files_dir / "memory_db" / member.key, member.value.text);
            }
        }
        if (const Value* meta = field(*memory, "meta"); meta != nullptr) {
            write_text_file(files_dir / "memory_db" / "memory_meta.json", meta->text);
        }
    }
    if (const Value* skills = field(scenario, "skills"); skills != nullptr && skills->kind == Kind::Object) {
        if (const Value* files = field(*skills, "files"); files != nullptr && files->kind == Kind::Object) {
            for (const Member& member : files->members) {
                write_text_file(files_dir / "skill_db" / member.key, member.value.text);
            }
        }
        if (const Value* meta = field(*skills, "meta"); meta != nullptr) {
            write_text_file(files_dir / "skill_db" / "skill_meta.json", meta->text);
        }
    }

    // ---- inputs -------------------------------------------------------------------------------
    const archive::SettingsSnapshot settings = read_snapshot(scenario);
    const auto conversation_settings = read_conversation_settings_map(scenario);
    const auto system_prompts = read_system_prompts(scenario);

    ScenarioSecrets secrets;
    read_secrets_scenario(scenario, secrets);
    secrets.shell_devices_ref = &settings.shell_devices;
    secrets.embedding_models_ref = &settings.embedding_models;
    secrets.mcp_servers_ref = &settings.mcp_servers;

    agora::core::memory::MemoryManager memory(files_dir.string());
    agora::core::skill::SkillManager skill(files_dir.string());

    data_export::Inputs inputs{settings, conversation_settings, system_prompts, memory, skill,
                               &secrets};

    // ---- database + store ------------------------------------------------------------------------
    SqliteExecutor sql(schema_path, scenario, root.string());
    agora::core::chat_dao::ChatDao dao(sql);
    export_snapshot::ChatDaoSnapshotStore store(dao, sql);

    // ---- steps -----------------------------------------------------------------------------------
    const Value* steps = field(scenario, "steps");
    if (steps == nullptr || steps->kind != Kind::Array) scenario_fail("scenario has no steps");
    std::vector<json::Value> trace_steps;
    // Kotlin's `outputs` list only receives available destinations (an "unavailable" step does
    // not append), and baselineStep indexes into that append order.
    std::vector<std::filesystem::path> outputs;

    for (std::size_t index = 0; index < steps->elements.size(); ++index) {
        const Value& step = steps->elements[index];
        if (required_string(step, "op") != "export") scenario_fail("unknown step op");

        // preSql mutates the database before this export (scenario 06).
        if (const Value* pre_sql = field(step, "preSql"); pre_sql != nullptr) {
            if (pre_sql->kind != Kind::Array) scenario_fail("preSql must be an array");
            for (const Value& statement : pre_sql->elements) {
                sql.execute(statement.text, {});
            }
        }

        // Baseline file: a previous step's output, optionally stamped or corrupted first.
        std::optional<std::filesystem::path> baseline_file;
        if (const std::optional<int> baseline_step = optional_int(step, "baselineStep");
            baseline_step.has_value()) {
            if (*baseline_step < 0 ||
                static_cast<std::size_t>(*baseline_step) >= outputs.size()) {
                scenario_fail("baselineStep has no output");
            }
            baseline_file = outputs[static_cast<std::size_t>(*baseline_step)];
            if (optional_flag(step, "corruptBaseline", false)) {
                std::ofstream stream(*baseline_file, std::ios::binary | std::ios::trunc);
                stream << "not a zip";
            } else if (optional_flag(step, "stampBaseline", false)) {
                stamp_baseline(*baseline_file);
            }
        }

        // Destination: "unavailable" makes openOutputStream return null in the stand.
        const bool unavailable =
            optional_string(step, "output").value_or("") == "unavailable";
        std::optional<std::filesystem::path> output_file;
        if (!unavailable) {
            output_file = out_dir / ("step-" + std::to_string(index) + ".zip");
            outputs.push_back(*output_file);
        }
        platform.set_output(output_file);

        data_export::ExportRequest request;
        for (const std::string& key : string_array(field(step, "categories"))) {
            const auto category = data_export::category_from_manifest_key(key);
            if (!category.has_value()) scenario_fail("unknown category " + key);
            request.categories.push_back(*category);
        }
        request.include_api_keys = optional_flag(step, "includeApiKeys", false);
        request.baseline_file = baseline_file;

        const ExportStepOutcome outcome =
            run_export(request, store, inputs, platform);

        json::Value result;
        if (outcome.failed) {
            result = json_object({
                {"error", json_object({
                              {"type", json_string(outcome.error_type)},
                              {"message", json_string(outcome.error_message)},
                          })},
            });
        } else {
            result = json_object({
                {"imagesExported", json_long(outcome.result.images_exported)},
                {"missingResourceCount", json_long(outcome.result.missing_resource_count)},
            });
        }

        json::Value archive = json_array({});
        if (output_file.has_value() && std::filesystem::is_regular_file(*output_file)) {
            zip::Reader reader(output_file->string());
            archive = render_archive(reader.entries());
        }

        std::vector<json::Value> progress;
        for (const std::string& value : outcome.progress) progress.push_back(json_string(value));

        trace_steps.push_back(json_object({
            {"op", json_string("export")},
            {"progress", json_array(std::move(progress))},
            {"result", std::move(result)},
            {"archive", std::move(archive)},
            {"cacheFileCount", json_long(static_cast<std::int64_t>(count_files(cache_dir)))},
        }));
    }

    const json::Value actual = json_object({{"steps", json_array(std::move(trace_steps))}});

    const json::ParseResult golden = json::parse(read_file(golden_path));
    if (!golden.value) scenario_fail("golden trace is not JSON: " + golden_path.string());
    if (!json_equal(actual, *golden.value)) {
        scenario_fail(scenario_path.filename().string() + "\n  expected: " +
                      excerpt(*golden.value) + "\n  actual:   " + excerpt(actual));
    }
}

void test_data_exporter_golden_parity() {
    const std::filesystem::path root = AGORA_FIXTURE_DATA_EXPORTER_DIR;
    const std::filesystem::path scenario_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            scenarios.push_back(entry.path());
        }
    }
    std::sort(scenarios.begin(), scenarios.end());
    if (scenarios.empty()) scenario_fail("no data-exporter scenarios found");
    for (const std::filesystem::path& scenario : scenarios) {
        const std::filesystem::path golden = golden_dir / (scenario.stem().string() + ".trace");
        run_scenario(scenario, golden, root / "platform" / "schema.sql");
    }
}

}  // namespace

TEST(data_exporter_golden_parity) { test_data_exporter_golden_parity(); }
