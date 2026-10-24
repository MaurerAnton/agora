#include "agora/core/data_exporter.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <map>
#include <memory>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/loop_policy.hpp"
#include "agora/core/native_backup_format.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/zip_reader.hpp"
#include "agora/core/zip_writer.hpp"

namespace agora::core::data_export {

namespace {

// -----------------------------------------------------------------------------------------------
// Helpers
// -----------------------------------------------------------------------------------------------

/** Kotlin String.isBlank(): true if empty or only whitespace (Character.isWhitespace). */
bool is_blank(std::string_view s) {
    for (char c : s) {
        if (!json::is_kotlin_whitespace_code_point(static_cast<unsigned char>(c))) {
            return false;
        }
    }
    return true;
}

/** Kotlin's mediaSourceKey logic. */
std::string media_source_key(std::string_view source, const Platform& platform) {
    std::string raw = std::string(source);
    if (raw.rfind("file://", 0) == 0) {
        raw = raw.substr(7);
    }
    if (source.rfind("content://", 0) == 0) {
        return std::string(source);
    }
    if (platform.file_exists(raw)) {
        try {
            return platform.canonical_path(raw);
        } catch (...) {
            return platform.absolute_path(raw);
        }
    }
    return std::string(source);
}

/** Kotlin's archiveMediaEntry logic: extension from source (strip query/fragment, lowercase). */
std::string archive_media_entry(std::string_view prefix, std::string_view source,
                                const std::function<std::string()>& new_uuid) {
    std::string without_query = std::string(source.substr(0,
        std::min(source.find('?'), source.find('#'))));
    std::string ext;
    auto dot = without_query.rfind('.');
    if (dot != std::string::npos && dot + 1 < without_query.size()) {
        ext = without_query.substr(dot + 1);
        if (ext.size() >= 1 && ext.size() <= 10) {
            bool all_alnum = true;
            for (char c : ext) {
                if (!std::isalnum(static_cast<unsigned char>(c))) {
                    all_alnum = false;
                    break;
                }
            }
            if (all_alnum) {
                for (char& c : ext) c = std::tolower(static_cast<unsigned char>(c));
            } else {
                ext.clear();
            }
        } else {
            ext.clear();
        }
    }
    std::string name = std::string(prefix) + new_uuid();
    if (!ext.empty()) {
        name += '.';
        name += ext;
    }
    return name;
}

/** Kotlin openImageStream: parse URI scheme, delegate to ContentResolver or file. */
std::optional<std::string> open_image_stream(std::string_view source, const Platform& platform) {
    std::string s(source);
    std::string scheme;
    auto colon = s.find(':');
    if (colon != std::string::npos && colon > 0) {
        bool valid = std::isalpha(static_cast<unsigned char>(s[0]));
        for (size_t i = 1; i < colon && valid; ++i) {
            char c = s[i];
            if (!std::isalnum(static_cast<unsigned char>(c)) && c != '+' && c != '-' && c != '.') {
                valid = false;
            }
        }
        if (valid) {
            scheme = s.substr(0, colon);
            for (char& c : scheme) c = std::tolower(static_cast<unsigned char>(c));
        }
    }
    if (scheme == "content" || scheme == "file") {
        return platform.open_media(source);
    }
    if (platform.file_exists(s)) {
        return platform.open_media(s);
    }
    return std::nullopt;
}

/** Float formatting matching Kotlin Float.toString(). */
std::string format_kotlin_float(float f) {
    return openai::format_kotlin_float(f);
}

/** Writes a JSON string member. */
void write_string_member(std::string& out, std::string_view key, std::string_view value, bool& first) {
    if (!first) out += ',';
    out += '"';
    json::escape_to(out, key);
    out += "\":\"";
    json::escape_to(out, value);
    out += '"';
    first = false;
}

/** Writes an optional JSON string member (omits if nullopt or empty and default). */
void write_optional_string_member(std::string& out, std::string_view key,
                                  const std::optional<std::string>& value, bool& first,
                                  bool omit_when_empty = false) {
    if (value.has_value() && (!omit_when_empty || !value->empty())) {
        write_string_member(out, key, *value, first);
    }
}

/** Writes an optional JSON int member. */
void write_optional_int_member(std::string& out, std::string_view key,
                               const std::optional<int>& value, bool& first) {
    if (value.has_value()) {
        if (!first) out += ',';
        out += '"';
        json::escape_to(out, key);
        out += "\":";
        out += std::to_string(*value);
        first = false;
    }
}

/** Writes an optional JSON float member (Kotlin Float.toString). */
void write_optional_float_member(std::string& out, std::string_view key,
                                 const std::optional<float>& value, bool& first) {
    if (value.has_value()) {
        if (!first) out += ',';
        out += '"';
        json::escape_to(out, key);
        out += "\":";
        out += format_kotlin_float(*value);
        first = false;
    }
}

/** Writes an optional JSON bool member. */
void write_optional_bool_member(std::string& out, std::string_view key,
                                const std::optional<bool>& value, bool& first) {
    if (value.has_value()) {
        if (!first) out += ',';
        out += '"';
        json::escape_to(out, key);
        out += "\":";
        out += (*value ? "true" : "false");
        first = false;
    }
}

/** Writes a JSON array of strings. */
void write_string_array_member(std::string& out, std::string_view key,
                               const std::vector<std::string>& arr, bool& first) {
    if (!arr.empty()) {
        if (!first) out += ',';
        out += '"';
        json::escape_to(out, key);
        out += "\":[";
        for (size_t i = 0; i < arr.size(); ++i) {
            if (i) out += ',';
            out += '"';
            json::escape_to(out, arr[i]);
            out += '"';
        }
        out += ']';
        first = false;
    }
}

/** Encodes a PromptTemplateItem. */
std::string encode_prompt_template_item(const PromptTemplateItem& item) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", item.id, first);
    write_string_member(out, "type", item.type, first);
    write_string_member(out, "value", item.value, first);
    out += "}";
    return out;
}

/** Encodes a SystemPromptEntry. */
std::string encode_system_prompt_entry(const SystemPromptEntry& entry) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", entry.id, first);
    write_string_member(out, "title", entry.title, first);
    if (!entry.content.empty()) {
        write_string_member(out, "content", entry.content, first);
    }
    auto write_items = [&](std::string_view key, const std::vector<PromptTemplateItem>& items) {
        if (!items.empty()) {
            if (!first) out += ',';
            out += '"';
            json::escape_to(out, key);
            out += "\":[";
            for (size_t i = 0; i < items.size(); ++i) {
                if (i) out += ',';
                out += encode_prompt_template_item(items[i]);
            }
            out += ']';
            first = false;
        }
    };
    write_items("systemItems", entry.system_items);
    write_items("userItems", entry.user_items);
    write_items("assistantItems", entry.assistant_items);
    write_items("userPrependItems", entry.user_prepend_items);
    write_items("userPostpendItems", entry.user_postpend_items);
    out += "}";
    return out;
}

/** Encodes a list of SystemPromptEntry. */
std::string encode_system_prompts(const std::vector<SystemPromptEntry>& prompts) {
    std::string out = "[";
    for (size_t i = 0; i < prompts.size(); ++i) {
        if (i) out += ',';
        out += encode_system_prompt_entry(prompts[i]);
    }
    out += "]";
    return out;
}

// Memory entry constants (matching Kotlin DataExporter)
inline constexpr std::string_view kActiveMemoryEntry = "memories/active_memory.md";
inline constexpr std::string_view kMemoryDbPrefix = "memories/memory_db/";
inline constexpr std::string_view kMemoryMetaEntry = "memories/memory_db/memory_meta.json";
inline constexpr std::string_view kSkillDbPrefix = "memories/skill_db/";
inline constexpr std::string_view kSkillMetaEntry = "memories/skill_db/skill_meta.json";

/** Kotlin Constants.PROVIDER_GOOGLE — ApiKeyEntry's default provider. */
inline constexpr std::string_view kProviderGoogle = "Google";

/** Writes a kotlinx map<string, string> object in insertion order. */
void write_string_map(std::string& out, std::string_view key,
                      const native_backup::StringMap& map, bool& first) {
    if (map.empty()) return;
    if (!first) out += ',';
    out += '"';
    json::escape_to(out, key);
    out += "\":{";
    bool first_entry = true;
    for (const auto& [map_key, map_value] : map) {
        if (!first_entry) out += ',';
        first_entry = false;
        out += '"';
        json::escape_to(out, map_key);
        out += "\":\"";
        json::escape_to(out, map_value);
        out += '"';
    }
    out += '}';
    first = false;
}

/**
 * kotlinx encoding of NativeBackupSecrets with encodeDefaults = false: every field has an empty
 * default, so only non-empty members appear, in declaration order.
 */
std::string encode_native_backup_secrets(const native_backup::NativeBackupSecrets& secrets) {
    std::string out = "{";
    bool first = true;
    if (!secrets.api_keys.empty()) {
        if (!first) out += ',';
        out += "\"apiKeys\":[";
        for (size_t i = 0; i < secrets.api_keys.size(); ++i) {
            if (i) out += ',';
            const auto& entry = secrets.api_keys[i];
            std::string item = "{";
            bool item_first = true;
            write_string_member(item, "id", entry.id, item_first);
            write_string_member(item, "name", entry.name, item_first);
            write_string_member(item, "key", entry.key, item_first);
            if (entry.provider != kProviderGoogle) {
                write_string_member(item, "provider", entry.provider, item_first);
            }
            item += "}";
            out += item;
        }
        out += ']';
        first = false;
    }
    write_string_map(out, "activeApiKeyIds", secrets.active_api_key_ids, first);
    write_string_map(out, "webSearchApiKeys", secrets.web_search_api_keys, first);
    if (!secrets.proxy_password.empty()) {
        write_string_member(out, "proxyPassword", secrets.proxy_password, first);
    }
    if (!secrets.shell_devices.empty()) {
        if (!first) out += ',';
        out += "\"shellDevices\":{";
        bool first_entry = true;
        for (const auto& [device_id, device_secrets] : secrets.shell_devices) {
            if (!first_entry) out += ',';
            first_entry = false;
            out += '"';
            json::escape_to(out, device_id);
            out += "\":{";
            bool item_first = true;
            if (!device_secrets.api_key.empty()) {
                write_string_member(out, "apiKey", device_secrets.api_key, item_first);
            }
            if (!device_secrets.ssh_password.empty()) {
                // sshPassword comes after apiKey; item_first tracks the inner object only.
                write_string_member(out, "sshPassword", device_secrets.ssh_password, item_first);
            }
            out += '}';
        }
        out += '}';
        first = false;
    }
    write_string_map(out, "shellApiKeys", secrets.shell_api_keys, first);
    write_string_map(out, "embeddingApiKeys", secrets.embedding_api_keys, first);
    if (!secrets.mcp_headers.empty()) {
        if (!first) out += ',';
        out += "\"mcpHeaders\":{";
        bool first_entry = true;
        for (const auto& [server_id, headers] : secrets.mcp_headers) {
            if (!first_entry) out += ',';
            first_entry = false;
            out += '"';
            json::escape_to(out, server_id);
            out += "\":{";
            bool header_first = true;
            for (const auto& [header_name, header_value] : headers) {
                if (!header_first) out += ',';
                header_first = false;
                out += '"';
                json::escape_to(out, header_name);
                out += "\":\"";
                json::escape_to(out, header_value);
                out += '"';
            }
            out += '}';
        }
        out += '}';
        first = false;
    }
    out += "}";
    return out;
}

/** Encodes ConversationSettings (export_extra_settings). */
std::string encode_conversation_settings(const export_extra_settings::ConversationSettings& s) {
    std::string out = "{";
    bool first = true;
    write_optional_int_member(out, "contextWindow", s.context_window, first);
    write_optional_float_member(out, "temperature", s.temperature, first);
    write_optional_int_member(out, "maxTokens", s.max_tokens, first);
    write_optional_float_member(out, "topP", s.top_p, first);
    write_optional_float_member(out, "frequencyPenalty", s.frequency_penalty, first);
    write_optional_float_member(out, "presencePenalty", s.presence_penalty, first);
    write_optional_bool_member(out, "codeExecutionEnabled", s.code_execution_enabled, first);
    write_optional_bool_member(out, "googleSearchEnabled", s.google_search_enabled, first);
    write_optional_bool_member(out, "openAiWebSearchEnabled", s.open_ai_web_search_enabled, first);
    write_optional_bool_member(out, "thinkingEnabled", s.thinking_enabled, first);
    write_optional_string_member(out, "thinkingLevel", s.thinking_level, first, true);
    write_optional_bool_member(out, "thinkingBudgetEnabled", s.thinking_budget_enabled, first);
    write_optional_int_member(out, "thinkingBudgetTokens", s.thinking_budget_tokens, first);
    write_optional_bool_member(out, "openAiServiceTierEnabled", s.open_ai_service_tier_enabled, first);
    write_optional_string_member(out, "openAiServiceTier", s.open_ai_service_tier, first, true);
    write_optional_bool_member(out, "webSearchEnabled", s.web_search_enabled, first);
    write_optional_bool_member(out, "shellEnabled", s.shell_enabled, first);
    write_optional_bool_member(out, "lowContextModeEnabled", s.low_context_mode_enabled, first);
    out += "}";
    return out;
}

/** Sanitizes a LoopEntity for export (port of DataImporter.sanitizeImportedLoop). */
conversation_export_snapshot::LoopEntity sanitize_imported_loop(
    const conversation_export_snapshot::LoopEntity& loop) {
    conversation_export_snapshot::LoopEntity out = loop;
    out.prompt = loop_policy::normalize_prompt(loop.prompt);
    out.cycle_count = std::max<int64_t>(loop.cycle_count, 0);
    int max_cycles = loop_policy::kDefaultMaxCycles;
    if (loop.max_cycles.has_value()) {
        int mc = *loop.max_cycles;
        if (mc >= loop_policy::kMinMaxCycles && mc <= loop_policy::kMaxMaxCycles) {
            max_cycles = mc;
        }
    }
    out.max_cycles = max_cycles;
    out.active = false;
    out.next_fire_at = 0;
    return out;
}

/**
 * Strict kotlinx decoder for List<SelectedAttachment> (DataExporter's default Json — unknown
 * keys, wrong types and missing required fields all throw, which the caller maps to nullopt the
 * same way Kotlin's runCatching.getOrNull() does). localId defaults to a fresh UUID.
 */
std::optional<backup::SelectedAttachment> decode_selected_attachment(const json::Value& v,
                                                                     const Platform& platform) {
    if (v.kind != json::Kind::Object) return std::nullopt;
    backup::SelectedAttachment a;
    std::optional<std::string> uri, type;
    bool local_id_seen = false;
    std::vector<std::string> known = {
        "localId", "uri", "type", "frameCount", "sliceIntervalMs", "pageCount",
        "videoDurationMs", "fileName", "mimeType", "fileSize", "pixelWidth", "pixelHeight",
        "processedFrames", "selectedPages", "preRenderedPaths", "localPath", "storage",
        "sandboxPath", "importState", "preparedText", "unavailable",
    };
    try {
        for (const auto& member : v.members) {
            if (std::find(known.begin(), known.end(), member.key) == known.end()) {
                return std::nullopt;  // kotlinx: unknown key throws
            }
            const json::Value& value = member.value;
            auto str = [&](std::optional<std::string>& out) -> bool {
                if (value.kind == json::Kind::String) { out = value.text; return true; }
                if (value.kind == json::Kind::Literal && value.text == "null") return true;
                return false;
            };
            auto opt_int = [&](std::optional<int>& out) -> bool {
                if (value.kind == json::Kind::Literal && value.text == "null") return true;
                if (value.kind != json::Kind::Literal) return false;
                size_t pos = 0;
                int parsed = std::stoi(value.text, &pos);
                if (pos != value.text.size()) return false;
                out = parsed;
                return true;
            };
            auto opt_long = [&](std::optional<std::int64_t>& out) -> bool {
                if (value.kind == json::Kind::Literal && value.text == "null") return true;
                if (value.kind != json::Kind::Literal) return false;
                size_t pos = 0;
                long long parsed = std::stoll(value.text, &pos);
                if (pos != value.text.size()) return false;
                out = parsed;
                return true;
            };
            auto string_list = [&](std::optional<std::vector<std::string>>& out) -> bool {
                if (value.kind == json::Kind::Literal && value.text == "null") return true;
                if (value.kind != json::Kind::Array) return false;
                std::vector<std::string> list;
                for (const auto& e : value.elements) {
                    if (e.kind != json::Kind::String) return false;
                    list.push_back(e.text);
                }
                out = list;
                return true;
            };
            bool ok = true;
            if (member.key == "localId") {
                // Non-nullable String: only a JSON string decodes; explicit null/errors throw.
                if (value.kind != json::Kind::String) return std::nullopt;
                a.local_id = value.text;
                local_id_seen = true;
            }
            else if (member.key == "uri") ok = str(uri);
            else if (member.key == "type") ok = str(type);
            else if (member.key == "frameCount") ok = opt_int(a.frame_count);
            else if (member.key == "sliceIntervalMs") ok = opt_long(a.slice_interval_ms);
            else if (member.key == "pageCount") ok = opt_int(a.page_count);
            else if (member.key == "videoDurationMs") ok = opt_long(a.video_duration_ms);
            else if (member.key == "fileName") ok = str(a.file_name);
            else if (member.key == "mimeType") ok = str(a.mime_type);
            else if (member.key == "fileSize") ok = opt_long(a.file_size);
            else if (member.key == "pixelWidth") ok = opt_int(a.pixel_width);
            else if (member.key == "pixelHeight") ok = opt_int(a.pixel_height);
            else if (member.key == "processedFrames") ok = string_list(a.processed_frames);
            else if (member.key == "selectedPages") {
                if (value.kind == json::Kind::Literal && value.text == "null") ok = true;
                else if (value.kind != json::Kind::Array) ok = false;
                else {
                    std::vector<int> pages;
                    for (const auto& e : value.elements) {
                        if (e.kind != json::Kind::Literal) { ok = false; break; }
                        pages.push_back(std::stoi(e.text));
                    }
                    if (ok) a.selected_pages = pages;
                }
            }
            else if (member.key == "preRenderedPaths") ok = string_list(a.pre_rendered_paths);
            else if (member.key == "localPath") ok = str(a.local_path);
            else if (member.key == "storage") {
                std::optional<std::string> s; ok = str(s);
                if (!ok || !s.has_value()) return std::nullopt;  // null -> kotlinx error
                if (*s == "app_private") a.storage = backup::AttachmentStorage::app_private;
                else if (*s == "local_sandbox_pending") a.storage = backup::AttachmentStorage::local_sandbox_pending;
                else if (*s == "local_sandbox_runtime") a.storage = backup::AttachmentStorage::local_sandbox_runtime;
                else return std::nullopt;
            }
            else if (member.key == "sandboxPath") ok = str(a.sandbox_path);
            else if (member.key == "importState") {
                std::optional<std::string> s; ok = str(s);
                if (!ok || !s.has_value()) return std::nullopt;  // null -> kotlinx error
                if (*s == "processing") a.import_state = backup::AttachmentImportState::processing;
                else if (*s == "ready") a.import_state = backup::AttachmentImportState::ready;
                else if (*s == "failed") a.import_state = backup::AttachmentImportState::failed;
                else return std::nullopt;
            }
            else if (member.key == "preparedText") ok = str(a.prepared_text);
            else if (member.key == "unavailable") {
                if (value.kind == json::Kind::Literal &&
                    (value.text == "true" || value.text == "false")) {
                    a.unavailable = value.text == "true";
                } else if (value.kind == json::Kind::Literal && value.text == "null") {
                    ok = false;
                } else {
                    ok = false;
                }
            }
            if (!ok) return std::nullopt;
        }
    } catch (const std::exception&) {
        return std::nullopt;
    }
    if (!uri.has_value() || !type.has_value()) {
        return std::nullopt;  // uri/type are required (missing -> kotlinx decode error)
    }
    if (!local_id_seen) {
        a.local_id = platform.new_uuid();  // Kotlin: `= UUID.randomUUID().toString()` default
    }
    a.uri = *uri;
    a.type = *type;
    return a;
}

std::optional<std::vector<backup::SelectedAttachment>> decode_selected_attachments(
    std::string_view raw, const Platform& platform) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != json::Kind::Array) return std::nullopt;
    std::vector<backup::SelectedAttachment> out;
    for (const auto& element : parsed.value->elements) {
        auto item = decode_selected_attachment(element, platform);
        if (!item) return std::nullopt;
        out.push_back(*item);
    }
    return out;
}

/** kotlinx List<SelectedAttachment> encoding (canonical). */
std::string encode_selected_attachments(const std::vector<backup::SelectedAttachment>& list) {
    std::string out = "[";
    for (size_t i = 0; i < list.size(); ++i) {
        if (i) out += ',';
        out += backup::encode_selected_attachment(list[i]);
    }
    out += "]";
    return out;
}

/** Rewrites a conversation's draftAttachments JSON for export. */
std::optional<std::string> rewrite_draft_attachments(
    const std::optional<std::string>& raw,
    const Platform& platform,
    const std::function<std::optional<std::string>(std::string_view)>& archive_entry,
    int& missing_resource_count) {
    if (!raw) return std::nullopt;
    auto attachments = decode_selected_attachments(*raw, platform);
    if (!attachments) return std::nullopt;  // decode failed: Kotlin keeps the original value
    auto rewritten = backup::NativeBackupMediaPolicy::rewrite_draft_attachments_for_export(
        *attachments, archive_entry,
        [&missing_resource_count]() { ++missing_resource_count; });
    if (rewritten.empty()) return std::nullopt;  // empty: Kotlin falls back to the original
    return encode_selected_attachments(rewritten);
}

/** Rewrites attachmentMeta JSON for export. */
std::optional<std::string> rewrite_attachment_meta(
    const std::optional<std::string>& raw,
    const std::vector<std::string>& original_image_sources,
    const std::map<int, int>& old_to_new_image_index,
    const std::function<std::optional<std::string>(std::string_view)>& archive_entry,
    int& missing_resource_count) {
    if (!raw) return std::nullopt;
    return backup::NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
        *raw, original_image_sources, old_to_new_image_index, archive_entry,
        [&missing_resource_count]() { ++missing_resource_count; });
}

/** Extracts tool image paths from toolCallJson. */
std::vector<std::string> extract_tool_image_paths(const std::optional<std::string>& raw) {
    if (!raw) return {};
    auto paths = backup::NativeBackupMediaPolicy::tool_image_paths(*raw);
    return paths;
}

/**
 * Encodes ExportChatEntity JSON. Field order and defaults match the Kotlin declaration:
 * id, title, lastUpdated, dataChangedAt, isPinned, selectedBranchesJson, systemPromptId,
 * modelId, taskId, origin, graduated, selectedRunBranchesJson, draftText, draftAttachments,
 * conversationSettings (encodeDefaults = false).
 */
std::string encode_export_chat_entity(
    const conversation_export_snapshot::ChatEntity& conv,
    const std::optional<export_extra_settings::ConversationSettings>& conv_settings,
    const std::optional<std::string>& draft_attachments) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", conv.id, first);
    write_string_member(out, "title", conv.title, first);
    write_optional_int_member(out, "lastUpdated", std::make_optional(conv.last_updated), first);
    // dataChangedAt defaults to 0 and is omitted there (encodeDefaults = false).
    if (conv.data_changed_at != 0) {
        write_optional_int_member(out, "dataChangedAt", std::make_optional(conv.data_changed_at),
                                  first);
    }
    if (conv.is_pinned) write_optional_bool_member(out, "isPinned", std::make_optional(true), first);
    write_optional_string_member(out, "selectedBranchesJson", conv.selected_branches_json, first);
    write_optional_string_member(out, "systemPromptId", conv.system_prompt_id, first);
    write_optional_string_member(out, "modelId", conv.model_id, first);
    write_optional_string_member(out, "taskId", conv.task_id, first);
    if (conv.origin != "user") write_string_member(out, "origin", conv.origin, first);
    if (conv.graduated) write_optional_bool_member(out, "graduated", std::make_optional(true), first);
    write_optional_string_member(out, "selectedRunBranchesJson", conv.selected_run_branches_json, first);
    if (!conv.draft_text.empty()) write_string_member(out, "draftText", conv.draft_text, first);
    if (draft_attachments.has_value()) {
        write_string_member(out, "draftAttachments", *draft_attachments, first);
    }
    if (conv_settings.has_value() && !conv_settings->is_all_null()) {
        if (!first) out += ',';
        out += "\"conversationSettings\":";
        out += encode_conversation_settings(*conv_settings);
        first = false;
    }
    out += "}";
    return out;
}

/**
 * Encodes ExportRunEntity JSON: id, conversationId, parentRunId, status, startedAt,
 * lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous.
 * status/startedAt/lastCheckpointAt have no default and are always encoded.
 */
std::string encode_export_run_entity(
    const conversation_export_snapshot::RunEntity& run) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", run.id, first);
    write_string_member(out, "conversationId", run.conversation_id, first);
    write_optional_string_member(out, "parentRunId", run.parent_run_id, first);
    write_string_member(out, "status",
        run.status == conversation_runtime::RunStatus::Active ? "ACTIVE" :
        run.status == conversation_runtime::RunStatus::Stopping ? "STOPPING" :
        run.status == conversation_runtime::RunStatus::Completed ? "COMPLETED" :
        run.status == conversation_runtime::RunStatus::Stopped ? "STOPPED" : "FAILED", first);
    write_optional_int_member(out, "startedAt", std::make_optional(run.started_at), first);
    write_optional_int_member(out, "lastCheckpointAt", std::make_optional(run.last_checkpoint_at), first);
    write_optional_int_member(out, "stopRequestedAt", run.stop_requested_at, first);
    write_optional_int_member(out, "endedAt", run.ended_at, first);
    write_optional_string_member(out, "endReason", run.end_reason ? std::make_optional(
        *run.end_reason == conversation_runtime::RunEndReason::ModelCompleted ? "MODEL_COMPLETED" :
        *run.end_reason == conversation_runtime::RunEndReason::UserStopped ? "USER_STOPPED" :
        *run.end_reason == conversation_runtime::RunEndReason::ProcessRecovered ? "PROCESS_RECOVERED" :
        "FAILED") : std::nullopt, first);
    if (run.current_pass != 0) write_optional_int_member(out, "currentPass", std::make_optional(run.current_pass), first);
    if (run.legacy_ambiguous) write_optional_bool_member(out, "legacyAmbiguous", std::make_optional(true), first);
    out += "}";
    return out;
}

/**
 * Encodes ExportMessageEntity JSON in Kotlin declaration order. Required fields (id,
 * conversationId, text, timestamp, runId, runSequence) are always encoded; defaults
 * (empty images, tokenCount 0, status SUCCESS, participant MODEL) are omitted.
 */
std::string encode_export_message_entity(
    const conversation_export_snapshot::MessageEntity& msg) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", msg.id, first);
    write_string_member(out, "conversationId", msg.conversation_id, first);
    write_optional_string_member(out, "parentId", msg.parent_id, first);
    write_string_member(out, "text", msg.text, first);
    if (!msg.images.empty()) {
        write_string_array_member(out, "images", msg.images, first);
    }
    write_optional_string_member(out, "thoughts", msg.thoughts, first);
    write_optional_string_member(out, "thoughtTitle", msg.thought_title, first);
    if (msg.token_count != 0) write_optional_int_member(out, "tokenCount", std::make_optional(msg.token_count), first);
    write_optional_int_member(out, "inputTokenCount", msg.input_token_count, first);
    write_optional_int_member(out, "cachedInputTokenCount", msg.cached_input_token_count, first);
    write_optional_int_member(out, "cacheWriteInputTokenCount", msg.cache_write_input_token_count, first);
    write_optional_int_member(out, "uncachedInputTokenCount", msg.uncached_input_token_count, first);
    write_optional_int_member(out, "outputTokenCount", msg.output_token_count, first);
    write_optional_int_member(out, "reasoningTokenCount", msg.reasoning_token_count, first);
    write_optional_int_member(out, "generationDurationMs", msg.generation_duration_ms, first);
    if (msg.status != run_recovery::MessageStatus::success) {
        write_string_member(out, "status",
            msg.status == run_recovery::MessageStatus::error ? "ERROR"
            : msg.status == run_recovery::MessageStatus::stopped ? "STOPPED"
            : msg.status == run_recovery::MessageStatus::thinking ? "THINKING"
            : msg.status == run_recovery::MessageStatus::tool_calling ? "TOOL_CALLING"
            : msg.status == run_recovery::MessageStatus::transcribing ? "TRANSCRIBING"
            : "SENDING", first);
    }
    if (msg.participant != run_recovery::Participant::model) {
        write_string_member(out, "participant",
            msg.participant == run_recovery::Participant::user ? "USER"
            : msg.participant == run_recovery::Participant::error ? "ERROR" : "MODEL", first);
    }
    write_optional_int_member(out, "timestamp", std::make_optional(msg.timestamp), first);
    write_optional_int_member(out, "thoughtTimeMs", msg.thought_time_ms, first);
    write_optional_string_member(out, "modelName", msg.model_name, first);
    write_optional_string_member(out, "toolCallJson", msg.tool_call_json, first);
    write_optional_string_member(out, "attachmentMeta", msg.attachment_meta, first);
    write_string_member(out, "runId", msg.run_id, first);
    write_optional_int_member(out, "runSequence", std::make_optional(msg.run_sequence), first);
    write_optional_int_member(out, "consumedAtPass", msg.consumed_at_pass, first);
    write_optional_string_member(out, "sourceJson", msg.source_json, first);
    out += "}";
    return out;
}

/**
 * Encodes ExportLoopEntity JSON: conversationId, intervalMs, prompt, cycleCount, maxCycles.
 * intervalMs has no default and is always encoded; maxCycles defaults to LoopPolicy's 10 and is
 * omitted when it equals it.
 */
std::string encode_export_loop_entity(
    const conversation_export_snapshot::LoopEntity& loop) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "conversationId", loop.conversation_id, first);
    write_optional_int_member(out, "intervalMs", std::make_optional(loop.interval_ms), first);
    write_optional_string_member(out, "prompt", loop.prompt, first);
    if (loop.cycle_count != 0) write_optional_int_member(out, "cycleCount", std::make_optional(loop.cycle_count), first);
    if (loop.max_cycles.has_value() && *loop.max_cycles != 10) {
        write_optional_int_member(out, "maxCycles", loop.max_cycles, first);
    }
    out += "}";
    return out;
}

/**
 * Encodes ExportTaskEntity JSON: id, name, prompt, systemPrompt, systemPromptId, modelId,
 * cronExpr, runAt, createdAt, lastRunAt. createdAt has no default and is always encoded.
 */
std::string encode_export_task_entity(
    const conversation_export_snapshot::TaskEntity& task) {
    std::string out = "{";
    bool first = true;
    write_string_member(out, "id", task.id, first);
    write_string_member(out, "name", task.name, first);
    write_string_member(out, "prompt", task.prompt, first);
    write_optional_string_member(out, "systemPrompt", task.system_prompt, first);
    write_optional_string_member(out, "systemPromptId", task.system_prompt_id, first);
    write_optional_string_member(out, "modelId", task.model_id, first);
    write_string_member(out, "cronExpr", task.cron_expr, first);
    write_optional_int_member(out, "runAt", task.run_at, first);
    write_optional_int_member(out, "createdAt", std::make_optional(task.created_at), first);
    write_optional_int_member(out, "lastRunAt", task.last_run_at, first);
    out += "}";
    return out;
}

}  // namespace

// -----------------------------------------------------------------------------------------------
// Context for archive phase (holds platform callbacks and state shared across conversations)
// -----------------------------------------------------------------------------------------------

struct ArchiveContext {
    const Platform& platform;
    zip::Writer& zip;
    export_spool::SpoolReader& spool;
    int& images_exported;
    int& missing_resource_count;
    /** sourceKey -> archive entry, shared across conversations (Kotlin's sourceToArchiveEntry). */
    std::unordered_map<std::string, std::string>& source_to_archive_entry;

    ArchiveContext(const Platform& p, zip::Writer& z, export_spool::SpoolReader& s,
                   int& img_exp, int& miss_cnt,
                   std::unordered_map<std::string, std::string>& src_map)
        : platform(p), zip(z), spool(s),
          images_exported(img_exp), missing_resource_count(miss_cnt),
          source_to_archive_entry(src_map) {}

    /**
     * Kotlin copySource: blank source -> nullopt; already-copied source -> existing entry;
     * otherwise open + stream into the zip (open failure = nullopt = missing resource, a read
     * failure after open propagates, like Kotlin's stream.copyTo throwing).
     */
    std::optional<std::string> copy_source(std::string_view source, std::string_view prefix,
                                           std::unordered_set<std::string>* referenced = nullptr) {
        if (is_blank(source)) return std::nullopt;
        std::string key = media_source_key(source, platform);
        auto it = source_to_archive_entry.find(key);
        if (it != source_to_archive_entry.end()) {
            if (referenced) referenced->insert(it->second);
            return it->second;
        }
        std::string entry = archive_media_entry(prefix, source, platform.new_uuid);
        auto bytes = open_image_stream(source, platform);
        if (!bytes) return std::nullopt;
        zip.put_entry(entry, *bytes);
        source_to_archive_entry[key] = entry;
        if (referenced) referenced->insert(entry);
        return entry;
    }

    /** Kotlin trackedLookup: only finds an entry already copied, recording it as referenced. */
    std::function<std::optional<std::string>(std::string_view)> tracked_lookup(
        std::unordered_set<std::string>& referenced) {
        return [this, &referenced](std::string_view source) -> std::optional<std::string> {
            std::string key = media_source_key(source, platform);
            auto it = source_to_archive_entry.find(key);
            if (it == source_to_archive_entry.end()) return std::nullopt;
            referenced.insert(it->second);
            return it->second;
        };
    }
};

// -----------------------------------------------------------------------------------------------
// Archive phase: writes one conversation's data to zip
// -----------------------------------------------------------------------------------------------

struct ConversationArchiveResult {
    native_backup_v5::ConversationIndexEntry index_entry;
};

// --- kotlinx re-encode helpers: substitute members of a parsed entity, preserving the
// --- declaration order kotlinx would re-emit them in (encodeDefaults = false already applied).

static json::Value make_string_value(std::string text) {
    json::Value v;
    v.kind = json::Kind::String;
    v.text = std::move(text);
    return v;
}

static json::Value make_string_array_value(const std::vector<std::string>& items) {
    json::Value v;
    v.kind = json::Kind::Array;
    for (const auto& item : items) v.elements.push_back(make_string_value(item));
    return v;
}

/** ExportChatEntity's Kotlin declaration order (for member insertion). */
static const std::vector<std::string>& chat_entity_key_order() {
    static const std::vector<std::string> order = {
        "id", "title", "lastUpdated", "dataChangedAt", "isPinned", "selectedBranchesJson",
        "systemPromptId", "modelId", "taskId", "origin", "graduated",
        "selectedRunBranchesJson", "draftText", "draftAttachments", "conversationSettings",
    };
    return order;
}

/** ExportMessageEntity's Kotlin declaration order (for member insertion). */
static const std::vector<std::string>& message_entity_key_order() {
    static const std::vector<std::string> order = {
        "id", "conversationId", "parentId", "text", "images", "thoughts", "thoughtTitle",
        "tokenCount", "inputTokenCount", "cachedInputTokenCount", "cacheWriteInputTokenCount",
        "uncachedInputTokenCount", "outputTokenCount", "reasoningTokenCount",
        "generationDurationMs", "status", "participant", "timestamp", "thoughtTimeMs",
        "modelName", "toolCallJson", "attachmentMeta", "runId", "runSequence",
        "consumedAtPass", "sourceJson",
    };
    return order;
}

static int declaration_index(const std::vector<std::string>& order, const std::string& key) {
    for (size_t i = 0; i < order.size(); ++i) {
        if (order[i] == key) return static_cast<int>(i);
    }
    return static_cast<int>(order.size());
}

/**
 * Substitutes one member of a parsed entity: replaces it in place, erases it when the value is
 * nullopt, or (value present, member absent) inserts it at the declaration position kotlinx's
 * re-encode would emit it at.
 */
static void set_member(json::Value& object, const std::string& key,
                       const std::optional<json::Value>& value,
                       const std::vector<std::string>& order) {
    int index = declaration_index(order, key);
    for (size_t i = 0; i < object.members.size(); ++i) {
        if (object.members[i].key == key) {
            if (value.has_value()) object.members[i].value = *value;
            else object.members.erase(object.members.begin() + static_cast<long>(i));
            return;
        }
    }
    if (!value.has_value()) return;
    size_t insert_at = object.members.size();
    for (size_t i = 0; i < object.members.size(); ++i) {
        if (declaration_index(order, object.members[i].key) > index) {
            insert_at = i;
            break;
        }
    }
    object.members.insert(object.members.begin() + static_cast<long>(insert_at),
                          json::Member{key, *value});
}

static std::optional<std::string> get_string_member(const json::Value& object,
                                                    const std::string& key) {
    for (const auto& member : object.members) {
        if (member.key == key && member.value.kind == json::Kind::String) {
            return member.value.text;
        }
    }
    return std::nullopt;
}

/**
 * Writes one conversation's slice to the archive: the media stream pass runs first (drafts from
 * the conversation record, videos and images per message, tool images), then the entry payload is
 * assembled from the collected records with images/toolCallJson/attachmentMeta rewritten exactly
 * like Kotlin's `entity.copy(...)` + re-encode.
 */
static ConversationArchiveResult write_conversation_archive(
    ArchiveContext& ctx,
    const export_spool::Slice& slice,
    const std::string& conversation_id,
    int64_t data_changed_at) {

    ConversationArchiveResult result;

    std::optional<std::string> conv_json_raw;
    std::vector<std::string> runs_json;
    std::vector<std::string> messages_json;
    std::vector<std::string> loops_json;

    std::optional<std::string> archived_draft_attachments;      // nullopt -> keep original
    std::unordered_map<std::string, std::vector<std::string>> archived_images_by_message;
    std::unordered_map<std::string, std::optional<std::string>> attachment_meta_by_message;
    std::unordered_set<std::string> referenced_media;  // per conversation (LinkedHashSet order)

    // Single in-order read over the slice; the media pass work happens exactly where Kotlin's
    // first pass does it, and the raw records are kept for the assembly pass.
    ctx.spool.read_records(slice, [&](const export_spool::SpoolReader::Record& record) {
        if (record.type == "C") {
            conv_json_raw = record.json;
            auto parsed = json::parse(record.json);
            if (!parsed.value || parsed.value->kind != json::Kind::Object) return;
            auto draft = get_string_member(*parsed.value, "draftAttachments");
            if (!draft) return;  // no draft: nothing to rewrite
            archived_draft_attachments = rewrite_draft_attachments(
                draft, ctx.platform,
                [&](std::string_view source) {
                    return ctx.copy_source(source, native_backup::kDraftMediaPrefix,
                                           &referenced_media);
                },
                ctx.missing_resource_count);
            // nullopt (decode failed or rewrote to empty) keeps the original in assembly.
            return;
        }
        if (record.type == "R") {
            runs_json.push_back(record.json);
            return;
        }
        if (record.type == "L") {
            loops_json.push_back(record.json);
            return;
        }
        if (record.type != "M") return;

        messages_json.push_back(record.json);
        auto parsed = json::parse(record.json);
        if (!parsed.value || parsed.value->kind != json::Kind::Object) return;
        const json::Value& obj = *parsed.value;

        std::vector<std::string> images;
        if (auto img_member = [&]() -> const json::Member* {
                for (const auto& m : obj.members) {
                    if (m.key == "images") return &m;
                }
                return nullptr;
            }()) {
            if (img_member->value.kind == json::Kind::Array) {
                for (const auto& e : img_member->value.elements) {
                    if (e.kind == json::Kind::String) images.push_back(e.text);
                }
            }
        }
        std::optional<std::string> attachment_meta = get_string_member(obj, "attachmentMeta");
        std::optional<std::string> tool_call_json = get_string_member(obj, "toolCallJson");
        std::optional<std::string> message_id = get_string_member(obj, "id");

        // Kotlin pass 1, message branch: videos first, then images, then tool images.
        if (attachment_meta) {
            if (auto meta = backup::decode_attachment_meta(*attachment_meta)) {
                for (const auto& item : meta->items) {
                    if (item.type == "video" && !item.unavailable && item.original_uri) {
                        ctx.copy_source(*item.original_uri, native_backup::kVideoMediaPrefix,
                                        nullptr);
                    }
                }
            }
        }

        if (!images.empty() && message_id.has_value()) {
            std::map<int, int> old_to_new_image_index;
            std::vector<std::string> archived;
            for (size_t i = 0; i < images.size(); ++i) {
                auto entry = ctx.copy_source(images[i], native_backup::kImageMediaPrefix,
                                             &referenced_media);
                if (entry) {
                    old_to_new_image_index[static_cast<int>(i)] =
                        static_cast<int>(archived.size());
                    archived.push_back(*entry);
                    ++ctx.images_exported;
                }
            }
            archived_images_by_message[*message_id] = archived;
            attachment_meta_by_message[*message_id] =
                backup::NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
                    attachment_meta, images, old_to_new_image_index,
                    ctx.tracked_lookup(referenced_media),
                    [&ctx]() { ++ctx.missing_resource_count; });
        } else if (attachment_meta) {
            attachment_meta_by_message[message_id.value_or("")] =
                backup::NativeBackupMediaPolicy::rewrite_attachment_meta_for_export(
                    attachment_meta, {}, {},
                    ctx.tracked_lookup(referenced_media),
                    [&ctx]() { ++ctx.missing_resource_count; });
        }

        for (const std::string& source :
             backup::NativeBackupMediaPolicy::tool_image_paths(tool_call_json)) {
            if (ctx.copy_source(source, native_backup::kImageMediaPrefix, nullptr)) {
                ++ctx.images_exported;
            }
        }
    });

    // Assembly pass: rewrite the conversation's draftAttachments and each message's
    // images/toolCallJson/attachmentMeta, then emit the entry.
    std::string conversation_json;
    if (conv_json_raw.has_value()) {
        json::ParseResult parsed = json::parse(*conv_json_raw);
        if (parsed.value && parsed.value->kind == json::Kind::Object) {
            json::Value& obj = *parsed.value;
            if (archived_draft_attachments.has_value()) {
                set_member(obj, "draftAttachments",
                           make_string_value(*archived_draft_attachments),
                           chat_entity_key_order());
            }
            conversation_json = json::to_kotlin_string(obj);
        } else {
            conversation_json = *conv_json_raw;
        }
    } else {
        conversation_json = "{}";  // unreachable: every slice starts with its C record
    }

    std::vector<std::string> rewritten_messages;
    for (const auto& raw : messages_json) {
        json::ParseResult parsed = json::parse(raw);
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            rewritten_messages.push_back(raw);
            continue;
        }
        json::Value& obj = *parsed.value;
        std::optional<std::string> message_id = get_string_member(obj, "id");

        auto images_it = archived_images_by_message.find(message_id.value_or(""));
        if (images_it != archived_images_by_message.end()) {
            if (images_it->second.empty()) {
                set_member(obj, "images", std::nullopt, message_entity_key_order());
            } else {
                set_member(obj, "images", make_string_array_value(images_it->second),
                           message_entity_key_order());
            }
        }

        if (get_string_member(obj, "toolCallJson").has_value()) {
            // Kotlin: rewriteToolImagePathsForExport(raw, trackedLookup) — unchanged when the
            // raw is blank or undecodable, replaced otherwise; null raw stays absent.
            std::optional<std::string> raw_tool = get_string_member(obj, "toolCallJson");
            std::optional<std::string> rewritten;
            if (raw_tool.has_value()) {
                if (json::is_blank(*raw_tool)) {
                    rewritten = *raw_tool;
                } else {
                    auto new_value = backup::NativeBackupMediaPolicy::rewrite_tool_image_paths_for_export(
                        raw_tool, ctx.tracked_lookup(referenced_media));
                    rewritten = new_value;
                }
            }
            if (rewritten.has_value()) {
                set_member(obj, "toolCallJson", make_string_value(*rewritten),
                           message_entity_key_order());
            } else {
                set_member(obj, "toolCallJson", std::nullopt, message_entity_key_order());
            }
        }

        auto meta_it = attachment_meta_by_message.find(message_id.value_or(""));
        if (meta_it != attachment_meta_by_message.end()) {
            if (meta_it->second.has_value()) {
                set_member(obj, "attachmentMeta", make_string_value(*meta_it->second),
                           message_entity_key_order());
            } else {
                set_member(obj, "attachmentMeta", std::nullopt, message_entity_key_order());
            }
        }

        rewritten_messages.push_back(json::to_kotlin_string(obj));
    }

    // The conversation's media entries (sorted) go into the index entry.
    std::vector<std::string> media_entries(referenced_media.begin(), referenced_media.end());
    result.index_entry = native_backup_v5::write_conversation_entry(
        ctx.zip, conversation_id, data_changed_at, conversation_json, runs_json,
        rewritten_messages, loops_json, std::move(media_entries));

    return result;
}

// -----------------------------------------------------------------------------------------------
// Spool writing phase
// -----------------------------------------------------------------------------------------------

struct ConversationSpoolData {
    std::string id;
    int64_t data_changed_at = 0;
    export_spool::Slice slice;
    bool in_spool = false;
};

struct SpoolResult {
    std::string spool_path;
    export_spool::Index index;
    std::vector<ConversationSpoolData> conversations;
    std::unordered_set<std::string> reused_conversation_ids;
    std::unordered_map<std::string, std::string> source_to_archive_entry;
    std::unordered_set<std::string> referenced_media;
    int images_exported = 0;
    int missing_resource_count = 0;
};

static SpoolResult write_spool_phase(
    const Platform& platform,
    conversation_export_snapshot::SnapshotStore& store,
    const Inputs& inputs,
    const ExportRequest& request,
    std::optional<native_backup_v5::Baseline>& baseline) {

    SpoolResult result;
    result.spool_path = platform.create_spool_file();
    export_spool::SpoolWriter writer(result.spool_path);

    bool include_conversations = std::find(request.categories.begin(), request.categories.end(),
                                           ExportCategory::Conversations) != request.categories.end();

    // Kotlin deletes the temp spool before rethrowing any capture failure; on success the caller
    // owns the file and removes it after the archive phase.
    bool capture_succeeded = false;
    struct WriterCleanup {
        export_spool::SpoolWriter& writer;
        const std::string& path;
        const bool& succeeded;
        ~WriterCleanup() {
            if (succeeded) return;
            try {
                writer.close();
            } catch (...) {
            }
            std::error_code ignored;
            std::filesystem::remove(path, ignored);
        }
    } writer_cleanup{writer, result.spool_path, capture_succeeded};

    store.begin_transaction();
    try {
        auto conversations = store.get_all_conversations_list();

        for (const auto& conv : conversations) {
            platform.ensure_active();

            bool reused = false;
            if (baseline && include_conversations) {
                if (baseline->can_reuse(conv.id, conv.data_changed_at)) {
                    reused = true;
                    result.reused_conversation_ids.insert(conv.id);
                }
            }

            // The reader always emits the conversation header; only its body is gated by
            // includeBody (which returns false for a reused conversation).
            std::string conv_json = encode_export_chat_entity(conv,
                inputs.conversation_settings.count(conv.id) ? 
                    std::make_optional(inputs.conversation_settings.at(conv.id)) : std::nullopt,
                conv.draft_attachments);
            writer.write_conversation(conv.id, conv.data_changed_at, conv_json);

            if (!reused && include_conversations) {
                // Write runs
                auto runs = store.get_runs_for_conversation_snapshot(conv.id);
                for (const auto& run : runs) {
                    platform.ensure_active();
                    writer.write_run(encode_export_run_entity(run));
                }

                // Write messages (paged)
                std::optional<std::string> after_id;
                while (true) {
                    platform.ensure_active();
                    auto messages = store.get_conversation_messages_page(conv.id, after_id,
                                                                       conversation_export_snapshot::kMessagePageSize);
                    if (messages.empty()) break;
                    for (const auto& msg : messages) {
                        writer.write_message(encode_export_message_entity(msg));
                    }
                    after_id = messages.back().id;
                    if (messages.size() < conversation_export_snapshot::kMessagePageSize) break;
                }

                // Write loops
                auto loops = store.get_loops_for_conversation_snapshot(conv.id);
                for (const auto& loop : loops) {
                    platform.ensure_active();
                    auto sanitized = sanitize_imported_loop(loop);
                    writer.write_loop(encode_export_loop_entity(sanitized));
                }
            }
        }

        // Write tasks (always when conversations category is selected)
        if (include_conversations) {
            auto tasks = store.get_all_tasks_list();
            for (const auto& task : tasks) {
                platform.ensure_active();
                writer.write_task(encode_export_task_entity(task));
            }
        }

    } catch (...) {
        store.end_transaction();
        throw;
    }
    store.set_transaction_successful();
    store.end_transaction();

    result.index = writer.finish();
    writer.close();

    // The writer's index is the spool layout: one slice per conversation header+body.
    for (const auto& idx_conv : result.index.conversations) {
        ConversationSpoolData cdata;
        cdata.id = idx_conv.id;
        cdata.data_changed_at = idx_conv.data_changed_at;
        cdata.slice = idx_conv.slice;
        cdata.in_spool = !result.reused_conversation_ids.count(idx_conv.id);
        result.conversations.push_back(std::move(cdata));
    }

    capture_succeeded = true;
    return result;
}

// -----------------------------------------------------------------------------------------------
// Main export function
// -----------------------------------------------------------------------------------------------

ExportResult export_data(conversation_export_snapshot::SnapshotStore& store,
                         const Inputs& inputs,
                         const Platform& platform,
                         const ExportRequest& request,
                         const ProgressCallback& on_progress) {

    bool has_conversations = std::find(request.categories.begin(), request.categories.end(),
                                       ExportCategory::Conversations) != request.categories.end();
    bool has_memories = std::find(request.categories.begin(), request.categories.end(),
                                  ExportCategory::Memories) != request.categories.end();
    bool has_system_prompts = std::find(request.categories.begin(), request.categories.end(),
                                        ExportCategory::SystemPrompts) != request.categories.end();
    bool has_settings = std::find(request.categories.begin(), request.categories.end(),
                                  ExportCategory::Settings) != request.categories.end();
    bool has_api_keys = std::find(request.categories.begin(), request.categories.end(),
                                  ExportCategory::ApiKeys) != request.categories.end();
    bool export_api_keys = has_api_keys && request.include_api_keys;

    // Kotlin: totalSteps = categories.size + 1; step() reports completed/totalSteps.
    const int total_steps = static_cast<int>(request.categories.size()) + 1;
    int completed = 0;
    auto step = [&]() {
        ++completed;
        if (on_progress) {
            on_progress(static_cast<float>(completed) / static_cast<float>(total_steps));
        }
    };

    // Kotlin opens the baseline only when the conversations category is selected.
    std::optional<native_backup_v5::Baseline> baseline;
    if (has_conversations && request.baseline_file.has_value()) {
        baseline = native_backup_v5::Baseline::open_or_null(*request.baseline_file);
    }

    // Kotlin captures the snapshot before opening the output stream; the spool file is deleted
    // on every exit path (the `finally` block), and a capture failure just rethrows.
    std::optional<SpoolResult> spool_result;
    if (has_conversations) {
        spool_result = write_spool_phase(platform, store, inputs, request, baseline);
    }
    struct SpoolCleanup {
        std::optional<SpoolResult>& result;
        ~SpoolCleanup() {
            if (result.has_value() && !result->spool_path.empty()) {
                std::error_code ignored;
                std::filesystem::remove(result->spool_path, ignored);
            }
        }
    } spool_cleanup{spool_result};

    // Kotlin: openOutputStream(uri) ?: throw IOException("Could not open ...").
    auto output_sink = platform.open_output();
    if (!output_sink) {
        throw IOException("Could not open the selected backup destination");
    }
    zip::Writer zip;

    // ---- Manifest (first entry) ---------------------------------------------------------------
    {
        std::string manifest = "{";
        manifest += "\"agora_export_version\":" + std::to_string(native_backup::kCurrentVersion);
        {
            std::string tmp;
            json::escape_to(tmp, platform.app_version());
            manifest += ",\"app_version\":\"" + tmp + "\"";
        }
        {
            std::string tmp;
            json::escape_to(tmp, platform.exported_at());
            manifest += ",\"exported_at\":\"" + tmp + "\"";
        }
        manifest += ",\"categories\":[";
        bool first_cat = true;
        for (ExportCategory cat : request.categories) {
            if (!first_cat) manifest += ',';
            std::string tmp;
            json::escape_to(tmp, manifest_key(cat));
            manifest += "\"" + tmp + "\"";
            first_cat = false;
        }
        manifest += "]";
        if (export_api_keys) manifest += ",\"has_api_keys\":true";
        // hasApiKeys (default false) and incrementalBaseline (default 1) are omitted otherwise:
        // kotlinx Json.encodeToStream runs with encodeDefaults = false.
        manifest += "}";
        zip.put_entry(native_backup::kManifestEntry, manifest);
    }
    step();

    // ---- Conversations -------------------------------------------------------------------------
    if (has_conversations) {
        export_spool::SpoolReader spool(spool_result->spool_path);
        ArchiveContext ctx(platform, zip, spool, spool_result->images_exported,
                          spool_result->missing_resource_count,
                          spool_result->source_to_archive_entry);

        std::vector<native_backup_v5::ConversationIndexEntry> index_entries;
        std::vector<std::string> copied_baseline_media;

        for (const auto& conv : spool_result->conversations) {
            platform.ensure_active();

            if (baseline && spool_result->reused_conversation_ids.count(conv.id)) {
                const native_backup_v5::ConversationIndexEntry* baseline_entry =
                    baseline->index_entry(conv.id);
                if (baseline_entry) {
                    index_entries.push_back(native_backup_v5::copy_conversation_from_baseline(
                        zip, *baseline, *baseline_entry, copied_baseline_media));
                    continue;
                }
            }

            index_entries.push_back(
                write_conversation_archive(ctx, conv.slice, conv.id, conv.data_changed_at)
                    .index_entry);
        }

        // Tasks entry, then the conversation index — both inside the conversations step.
        std::vector<std::string> tasks_json;
        for (const auto& task_slice : spool_result->index.task_slices) {
            spool.read_records(task_slice, [&](const export_spool::SpoolReader::Record& record) {
                if (record.type == "T") tasks_json.push_back(record.json);
            });
        }
        native_backup_v5::write_tasks_entry(zip, tasks_json);
        native_backup_v5::write_conversation_index(zip, index_entries);
        step();
    }

    // ---- Memories (active memory, memory files, memory meta, skills, skill meta) ---------------
    if (has_memories) {
        std::string active = inputs.memory.get_active_memory();
        if (!active.empty()) {
            zip.put_entry(kActiveMemoryEntry, active);
        }
        for (const auto& file : inputs.memory.list_files()) {
            zip.put_entry(std::string(kMemoryDbPrefix) + file.name,
                          inputs.memory.read_file(file.name));
        }
        std::string meta = inputs.memory.get_meta_json();
        if (meta != "{}") {
            zip.put_entry(kMemoryMetaEntry, meta);
        }
        for (const auto& file : inputs.skill.list_files()) {
            zip.put_entry(std::string(kSkillDbPrefix) + file.name,
                          inputs.skill.read_file(file.name));
        }
        std::string skill_meta = inputs.skill.get_meta_json();
        if (skill_meta != "{}") {
            zip.put_entry(kSkillMetaEntry, skill_meta);
        }
        step();
    }

    // ---- System prompts ------------------------------------------------------------------------
    if (has_system_prompts) {
        zip.put_entry(native_backup::kSystemPromptsEntry,
                      encode_system_prompts(inputs.system_prompts));
        step();
    }

    // ---- Settings (custom font first, then settings.json) --------------------------------------
    if (has_settings) {
        bool font_included = false;
        const std::string& font_path = inputs.settings.custom_font_path;
        if (!font_path.empty()) {
            std::error_code ec;
            if (std::filesystem::is_regular_file(font_path, ec)) {
                std::ifstream font_file(font_path, std::ios::binary);
                if (font_file) {
                    std::string content((std::istreambuf_iterator<char>(font_file)),
                                        std::istreambuf_iterator<char>());
                    zip.put_entry(native_backup::kCustomFontEntry, content);
                    font_included = true;
                }
            }
        }
        zip.put_entry(native_backup::kSettingsEntry,
                      json::to_kotlin_string(
                          settings_archive::to_json_object(inputs.settings, font_included)));
        step();
    }

    // ---- API keys (opt-in: includeApiKeys && category) ------------------------------------------
    if (export_api_keys) {
        if (!inputs.secrets_store) {
            throw std::logic_error(
                "data_export: Inputs::secrets_store is required to export the api_keys category");
        }
        zip.put_entry(native_backup::kSecretsEntry,
                      encode_native_backup_secrets(native_backup::capture(*inputs.secrets_store)));
        step();
    }

    // ---- Finish --------------------------------------------------------------------------------
    std::string archive_data = zip.finish();
    output_sink->write(archive_data);

    if (on_progress) on_progress(1.0f);

    return {spool_result ? spool_result->images_exported : 0,
            spool_result ? spool_result->missing_resource_count : 0};
}

}  // namespace agora::core::data_export