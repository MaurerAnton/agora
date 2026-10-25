#include "agora/core/native_conversation_graph_importer.hpp"

#include <algorithm>
#include <cctype>
#include <cstdint>
#include <set>
#include <stdexcept>
#include <utility>

#include "agora/core/backup_media_policy.hpp"
#include "agora/core/json.hpp"
#include "agora/core/legacy_run_backfill.hpp"
#include "agora/core/loop_policy.hpp"
#include "agora/core/message_source.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/native_run_archive.hpp"
#include "agora/core/regeneration_tree_repair.hpp"

namespace agora::core::native_graph_import {

using android_json::Exception;
using android_json::Reader;
using android_json::Token;
using automation::LoopEntity;
using automation::TaskEntity;
using json::Kind;
using json::Member;
using json::Value;

namespace {

// -----------------------------------------------------------------------------------------------
// kotlinx decode helpers for the injected `Json { ignoreUnknownKeys, coerceInputValues }` config
// ---------------------------------------------------------------------------------------------

const char* const kImporterSerialPrefix =
    "com.newoether.agora.data.NativeConversationGraphImporter.";

[[noreturn]] void missing_field(std::string_view entity, std::string_view field) {
    throw Exception("MissingFieldException",
                    "Field '" + std::string(field) + "' is required for type with serial name '" +
                        kImporterSerialPrefix + std::string(entity) +
                        "', but it was missing");
}

const Value* member_of(const Value& object, std::string_view key) {
    if (object.kind != Kind::Object) return nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == key) return &entry.value;  // last duplicate wins, like kotlinx
    }
    return nullptr;
}

std::string require_string(const Value& object, std::string_view key, std::string_view entity) {
    const Value* value = member_of(object, key);
    if (value == nullptr) missing_field(entity, key);
    if (value->kind != Kind::String) {
        throw Exception("SerializationException",
                        "Element " + std::string(key) + " is not a JsonPrimitive");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view key) {
    const Value* value = member_of(object, key);
    if (value == nullptr || (value->kind == Kind::Literal && value->text == "null")) {
        return std::nullopt;
    }
    if (value->kind != Kind::String) {
        throw Exception("SerializationException",
                        "Element " + std::string(key) + " is not a JsonPrimitive");
    }
    return value->text;
}

long long require_long(const Value& object, std::string_view key, std::string_view entity) {
    const Value* value = member_of(object, key);
    if (value == nullptr) missing_field(entity, key);
    if (value->kind != Kind::Literal || value->text == "null") {
        throw Exception("SerializationException",
                        "Element " + std::string(key) + " is not a number");
    }
    return std::stoll(value->text);
}

std::optional<long long> optional_long(const Value& object, std::string_view key) {
    const Value* value = member_of(object, key);
    if (value == nullptr || (value->kind == Kind::Literal && value->text == "null")) {
        return std::nullopt;
    }
    if (value->kind != Kind::Literal) {
        throw Exception("SerializationException",
                        "Element " + std::string(key) + " is not a number");
    }
    return std::stoll(value->text);
}

std::optional<int> optional_int(const Value& object, std::string_view key) {
    const std::optional<long long> value = optional_long(object, key);
    if (!value.has_value()) return std::nullopt;
    return static_cast<int>(*value);
}

bool optional_bool(const Value& object, std::string_view key, bool fallback) {
    const Value* value = member_of(object, key);
    if (value == nullptr || (value->kind == Kind::Literal && value->text == "null")) return fallback;
    if (value->kind == Kind::Literal && value->text == "true") return true;
    if (value->kind == Kind::Literal && value->text == "false") return false;
    throw Exception("SerializationException",
                    "Element " + std::string(key) + " is not a boolean");
}

std::vector<std::string> optional_string_array(const Value& object, std::string_view key) {
    std::vector<std::string> out;
    const Value* value = member_of(object, key);
    if (value == nullptr || (value->kind == Kind::Literal && value->text == "null")) return out;
    if (value->kind != Kind::Array) {
        throw Exception("SerializationException",
                        "Element " + std::string(key) + " is not an array");
    }
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) {
            throw Exception("SerializationException",
                            "Element " + std::string(key) + " is not a string array");
        }
        out.push_back(element.text);
    }
    return out;
}

// -----------------------------------------------------------------------------------------------
// readNativeGraphValue: JsonReader -> kotlinx JsonElement (json::Value)
// ---------------------------------------------------------------------------------------------

Value read_native_graph_value(Reader& reader) {
    switch (reader.peek()) {
        case Token::BeginObject: {
            Value object;
            object.kind = Kind::Object;
            reader.begin_object();
            while (reader.has_next()) {
                Member member;
                member.key = reader.next_name();
                member.value = read_native_graph_value(reader);
                object.members.push_back(std::move(member));
            }
            reader.end_object();
            return object;
        }
        case Token::BeginArray: {
            Value array;
            array.kind = Kind::Array;
            reader.begin_array();
            while (reader.has_next()) {
                array.elements.push_back(read_native_graph_value(reader));
            }
            reader.end_array();
            return array;
        }
        case Token::String: {
            Value out;
            out.kind = Kind::String;
            out.text = reader.next_string();
            return out;
        }
        case Token::Number: {
            const json::ParseResult parsed = json::parse(reader.next_string());
            if (!parsed.value) {
                throw Exception("SerializationException", "Unexpected JSON token");
            }
            return *parsed.value;
        }
        case Token::Boolean: {
            Value out;
            out.kind = Kind::Literal;
            out.text = reader.next_boolean() ? "true" : "false";
            return out;
        }
        case Token::Null: {
            reader.next_null();
            Value out;
            out.kind = Kind::Literal;
            out.text = "null";
            return out;
        }
        default:
            throw Exception("IllegalStateException",
                            std::string("Unexpected JSON token ") +
                                android_json::token_name(reader.peek()));
    }
}

/** Counts one field's array without deserializing it (countArray). */
int count_array(Reader& reader) {
    int count = 0;
    reader.begin_array();
    while (reader.has_next()) {
        reader.skip_value();
        ++count;
    }
    reader.end_array();
    return count;
}

// -----------------------------------------------------------------------------------------------
// Stored selections: Map<String?, String> <-> JSON with the "null" key
// ---------------------------------------------------------------------------------------------

using SelectionMap = std::vector<std::pair<std::optional<std::string>, std::string>>;

std::optional<SelectionMap> decode_stored_selections(const std::optional<std::string>& raw) {
    if (!raw.has_value() || json::is_blank(*raw)) return std::nullopt;
    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value || parsed.value->kind != Kind::Object) return std::nullopt;
    SelectionMap out;
    for (const Member& member : parsed.value->members) {
        if (member.value.kind != Kind::String) return std::nullopt;  // kotlinx type error
        out.emplace_back(member.key == "null" ? std::nullopt
                                              : std::optional<std::string>(member.key),
                         member.value.text);
    }
    return out;
}

std::string encode_stored_selections(const SelectionMap& selections) {
    std::string out = "{";
    bool first = true;
    for (const auto& [key, value] : selections) {
        if (!first) out += ',';
        first = false;
        out += '"';
        json::escape_to(out, key.has_value() ? *key : "null");
        out += "\":\"";
        json::escape_to(out, value);
        out += '"';
    }
    out += '}';
    return out;
}

// -----------------------------------------------------------------------------------------------
// Enum valueOf with the importer's try/catch fallbacks
// ---------------------------------------------------------------------------------------------

run_recovery::Participant parse_participant(const std::string& raw) {
    if (raw == "USER") return run_recovery::Participant::user;
    if (raw == "MODEL") return run_recovery::Participant::model;
    if (raw == "ERROR") return run_recovery::Participant::error;
    return run_recovery::Participant::model;
}

run_recovery::MessageStatus parse_message_status(const std::string& raw) {
    if (raw == "SUCCESS") return run_recovery::MessageStatus::success;
    if (raw == "SENDING") return run_recovery::MessageStatus::sending;
    if (raw == "THINKING") return run_recovery::MessageStatus::thinking;
    if (raw == "TOOL_CALLING") return run_recovery::MessageStatus::tool_calling;
    if (raw == "TRANSCRIBING") return run_recovery::MessageStatus::transcribing;
    if (raw == "STOPPED") return run_recovery::MessageStatus::stopped;
    if (raw == "ERROR") return run_recovery::MessageStatus::error;
    return run_recovery::MessageStatus::success;
}

// -----------------------------------------------------------------------------------------------
// sanitizeImported* (DataImporter.kt, lifted by the stand's extractor)
// ---------------------------------------------------------------------------------------------

TaskEntity sanitize_imported_task(TaskEntity task) {
    task.name = json::trim(task.name);
    task.prompt = json::trim(task.prompt);
    task.cron_expr = json::trim(task.cron_expr);
    task.next_run_at = 0;
    task.enabled = false;
    return task;
}

LoopEntity sanitize_imported_loop(const LoopEntity& loop) {
    LoopEntity out = loop;
    out.prompt = loop_policy::normalize_prompt(loop.prompt);
    out.cycle_count = std::max<long long>(loop.cycle_count, 0);
    int max_cycles = loop_policy::kDefaultMaxCycles;
    if (loop.max_cycles.has_value()) {
        const int value = *loop.max_cycles;
        if (value >= loop_policy::kMinMaxCycles && value <= loop_policy::kMaxMaxCycles) {
            max_cycles = value;
        }
    }
    out.max_cycles = max_cycles;
    out.active = false;
    out.next_fire_at = 0;
    return out;
}

ChatEntity sanitize_imported_conversation(ChatEntity conversation,
                                          const std::set<std::string>& available_task_ids) {
    conversation.has_unread_generation = false;
    if (conversation.task_id.has_value() &&
        available_task_ids.find(*conversation.task_id) == available_task_ids.end()) {
        conversation.task_id.reset();
        conversation.origin = "user";
        conversation.graduated = true;
    }
    return conversation;
}

}  // namespace

// -----------------------------------------------------------------------------------------------
// Export* entity decode (the importer's private @Serializable classes)
// -----------------------------------------------------------------------------------------------

std::optional<export_extra_settings::ConversationSettings> decode_conversation_settings(
    const Value& object) {
    if (object.kind != Kind::Object) {
        throw Exception("SerializationException",
                        "Element conversationSettings is not a JsonObject");
    }
    export_extra_settings::ConversationSettings settings;
    auto text = [&](std::string_view key) { return optional_string(object, key); };
    settings.context_window = optional_int(object, "contextWindow");
    if (const Value* v = member_of(object, "temperature")) {
        if (!(v->kind == Kind::Literal && v->text == "null")) settings.temperature = std::strtof(std::string(v->text).c_str(), nullptr);
    }
    settings.max_tokens = optional_int(object, "maxTokens");
    if (const Value* v = member_of(object, "topP")) {
        if (!(v->kind == Kind::Literal && v->text == "null")) settings.top_p = std::strtof(std::string(v->text).c_str(), nullptr);
    }
    if (const Value* v = member_of(object, "frequencyPenalty")) {
        if (!(v->kind == Kind::Literal && v->text == "null")) settings.frequency_penalty = std::strtof(std::string(v->text).c_str(), nullptr);
    }
    if (const Value* v = member_of(object, "presencePenalty")) {
        if (!(v->kind == Kind::Literal && v->text == "null")) settings.presence_penalty = std::strtof(std::string(v->text).c_str(), nullptr);
    }
    settings.code_execution_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "codeExecutionEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "codeExecutionEnabled", false);
    }();
    settings.google_search_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "googleSearchEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "googleSearchEnabled", false);
    }();
    settings.open_ai_web_search_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "openAiWebSearchEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "openAiWebSearchEnabled", false);
    }();
    settings.thinking_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "thinkingEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "thinkingEnabled", false);
    }();
    settings.thinking_level = text("thinkingLevel");
    settings.thinking_budget_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "thinkingBudgetEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "thinkingBudgetEnabled", false);
    }();
    settings.thinking_budget_tokens = optional_int(object, "thinkingBudgetTokens");
    settings.open_ai_service_tier_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "openAiServiceTierEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "openAiServiceTierEnabled", false);
    }();
    settings.open_ai_service_tier = text("openAiServiceTier");
    settings.web_search_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "webSearchEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "webSearchEnabled", false);
    }();
    settings.shell_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "shellEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "shellEnabled", false);
    }();
    settings.low_context_mode_enabled = [&]()->std::optional<bool>{
        const Value* v = member_of(object, "lowContextModeEnabled");
        if (!v || (v->kind == Kind::Literal && v->text == "null")) return std::nullopt;
        return optional_bool(object, "lowContextModeEnabled", false);
    }();
    return settings;
}

struct ExportChat {
    std::string id;
    std::string title;
    long long last_updated = 0;
    bool is_pinned = false;
    std::optional<std::string> selected_branches_json;
    std::optional<std::string> system_prompt_id;
    std::optional<std::string> model_id;
    std::optional<std::string> task_id;
    std::string origin = "user";
    bool graduated = false;
    std::optional<std::string> selected_run_branches_json;
    std::string draft_text;
    std::optional<std::string> draft_attachments;
    std::optional<export_extra_settings::ConversationSettings> conversation_settings;
    bool has_unread_generation = false;
};

ExportChat decode_export_chat(const Value& value) {
    if (value.kind != Kind::Object) {
        throw Exception("SerializationException", "Expected JSON object");
    }
    ExportChat out;
    out.id = require_string(value, "id", "ExportChatEntity");
    out.title = require_string(value, "title", "ExportChatEntity");
    out.last_updated = require_long(value, "lastUpdated", "ExportChatEntity");
    out.is_pinned = optional_bool(value, "isPinned", false);
    out.selected_branches_json = optional_string(value, "selectedBranchesJson");
    out.system_prompt_id = optional_string(value, "systemPromptId");
    out.model_id = optional_string(value, "modelId");
    out.task_id = optional_string(value, "taskId");
    out.origin = optional_string(value, "origin").value_or("user");
    out.graduated = optional_bool(value, "graduated", false);
    out.selected_run_branches_json = optional_string(value, "selectedRunBranchesJson");
    out.draft_text = optional_string(value, "draftText").value_or("");
    out.draft_attachments = optional_string(value, "draftAttachments");
    if (const Value* settings = member_of(value, "conversationSettings")) {
        if (!(settings->kind == Kind::Literal && settings->text == "null")) {
            out.conversation_settings = decode_conversation_settings(*settings);
        }
    }
    out.has_unread_generation = optional_bool(value, "hasUnreadGeneration", false);
    return out;
}

struct ExportRun {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    std::string status = "COMPLETED";
    long long started_at = 0;
    long long last_checkpoint_at = 0;
    std::optional<long long> stop_requested_at;
    std::optional<long long> ended_at;
    std::optional<std::string> end_reason;
    int current_pass = 0;
    bool legacy_ambiguous = false;
};

ExportRun decode_export_run(const Value& value) {
    if (value.kind != Kind::Object) {
        throw Exception("SerializationException", "Expected JSON object");
    }
    ExportRun out;
    out.id = require_string(value, "id", "ExportRunEntity");
    out.conversation_id = require_string(value, "conversationId", "ExportRunEntity");
    out.parent_run_id = optional_string(value, "parentRunId");
    out.status = optional_string(value, "status").value_or("COMPLETED");
    out.started_at = require_long(value, "startedAt", "ExportRunEntity");
    out.last_checkpoint_at = require_long(value, "lastCheckpointAt", "ExportRunEntity");
    out.stop_requested_at = optional_long(value, "stopRequestedAt");
    out.ended_at = optional_long(value, "endedAt");
    out.end_reason = optional_string(value, "endReason");
    out.current_pass = optional_int(value, "currentPass").value_or(0);
    out.legacy_ambiguous = optional_bool(value, "legacyAmbiguous", false);
    return out;
}

native_run_archive::ArchivedRunSnapshot to_archived_snapshot(const ExportRun& run) {
    native_run_archive::ArchivedRunSnapshot out;
    out.id = run.id;
    out.conversation_id = run.conversation_id;
    out.parent_run_id = run.parent_run_id;
    out.status = run.status;
    out.started_at = run.started_at;
    out.last_checkpoint_at = run.last_checkpoint_at;
    out.stop_requested_at = run.stop_requested_at;
    out.ended_at = run.ended_at;
    out.end_reason = run.end_reason;
    out.current_pass = run.current_pass;
    out.legacy_ambiguous = run.legacy_ambiguous;
    return out;
}

TaskEntity decode_export_task(const Value& value) {
    if (value.kind != Kind::Object) {
        throw Exception("SerializationException", "Expected JSON object");
    }
    TaskEntity out;
    out.id = require_string(value, "id", "ExportTaskEntity");
    out.name = require_string(value, "name", "ExportTaskEntity");
    out.prompt = require_string(value, "prompt", "ExportTaskEntity");
    out.system_prompt = optional_string(value, "systemPrompt");
    out.system_prompt_id = optional_string(value, "systemPromptId");
    out.model_id = optional_string(value, "modelId");
    out.cron_expr = require_string(value, "cronExpr", "ExportTaskEntity");
    out.run_at = optional_long(value, "runAt");
    out.next_run_at = optional_long(value, "nextRunAt").value_or(0);
    out.enabled = optional_bool(value, "enabled", true);
    out.created_at = require_long(value, "createdAt", "ExportTaskEntity");
    out.last_run_at = optional_long(value, "lastRunAt");
    return out;
}

LoopEntity decode_export_loop(const Value& value) {
    if (value.kind != Kind::Object) {
        throw Exception("SerializationException", "Expected JSON object");
    }
    LoopEntity out;
    out.conversation_id = require_string(value, "conversationId", "ExportLoopEntity");
    out.interval_ms = require_long(value, "intervalMs", "ExportLoopEntity");
    out.prompt = optional_string(value, "prompt");
    out.next_fire_at = optional_long(value, "nextFireAt").value_or(0);
    out.cycle_count = optional_int(value, "cycleCount").value_or(0);
    // Nullable with default: absent -> the default 10, explicit null -> null (sanitize maps
    // both to 10, but keep the distinction exactly as kotlinx decodes it).
    if (const Value* max_cycles = member_of(value, "maxCycles")) {
        out.max_cycles = optional_int(value, "maxCycles");
    } else {
        out.max_cycles = loop_policy::kDefaultMaxCycles;
    }
    out.active = optional_bool(value, "active", true);
    out.revision = optional_long(value, "revision").value_or(0);
    return out;
}

struct ExportMessage {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_id;
    std::string text;
    std::vector<std::string> images;
    std::optional<std::string> thoughts;
    std::optional<std::string> thought_title;
    int token_count = 0;
    std::optional<int> input_token_count;
    std::optional<int> cached_input_token_count;
    std::optional<int> cache_write_input_token_count;
    std::optional<int> uncached_input_token_count;
    std::optional<int> output_token_count;
    std::optional<int> reasoning_token_count;
    std::optional<long long> generation_duration_ms;
    std::string status = "SUCCESS";
    std::string participant = "MODEL";
    long long timestamp = 0;
    std::optional<long long> thought_time_ms;
    std::optional<std::string> model_name;
    std::optional<std::string> tool_call_json;
    std::optional<std::string> attachment_meta;
    std::optional<std::string> run_id;
    std::optional<long long> run_sequence;
    std::optional<int> consumed_at_pass;
    std::optional<std::string> source_json;
};

ExportMessage decode_export_message(const Value& value) {
    if (value.kind != Kind::Object) {
        throw Exception("SerializationException", "Expected JSON object");
    }
    ExportMessage out;
    out.id = require_string(value, "id", "ExportMessageEntity");
    out.conversation_id = require_string(value, "conversationId", "ExportMessageEntity");
    out.parent_id = optional_string(value, "parentId");
    out.text = require_string(value, "text", "ExportMessageEntity");
    out.images = optional_string_array(value, "images");
    out.thoughts = optional_string(value, "thoughts");
    out.thought_title = optional_string(value, "thoughtTitle");
    out.token_count = optional_int(value, "tokenCount").value_or(0);
    out.input_token_count = optional_int(value, "inputTokenCount");
    out.cached_input_token_count = optional_int(value, "cachedInputTokenCount");
    out.cache_write_input_token_count = optional_int(value, "cacheWriteInputTokenCount");
    out.uncached_input_token_count = optional_int(value, "uncachedInputTokenCount");
    out.output_token_count = optional_int(value, "outputTokenCount");
    out.reasoning_token_count = optional_int(value, "reasoningTokenCount");
    out.generation_duration_ms = optional_long(value, "generationDurationMs");
    out.status = optional_string(value, "status").value_or("SUCCESS");
    out.participant = optional_string(value, "participant").value_or("MODEL");
    out.timestamp = require_long(value, "timestamp", "ExportMessageEntity");
    out.thought_time_ms = optional_long(value, "thoughtTimeMs");
    out.model_name = optional_string(value, "modelName");
    out.tool_call_json = optional_string(value, "toolCallJson");
    out.attachment_meta = optional_string(value, "attachmentMeta");
    out.run_id = optional_string(value, "runId");
    out.run_sequence = optional_long(value, "runSequence");
    out.consumed_at_pass = optional_int(value, "consumedAtPass");
    out.source_json = optional_string(value, "sourceJson");
    return out;
}

// -----------------------------------------------------------------------------------------------
// One pass over the graph object: the four arrays (messages skipped unless requested)
// -----------------------------------------------------------------------------------------------

struct GraphArrays {
    std::vector<Value> conversations;
    std::vector<Value> runs;
    std::vector<Value> tasks;
    std::vector<Value> loops;
    std::vector<Value> messages;
};

GraphArrays collect_graph_arrays(Reader& reader, bool with_messages) {
    GraphArrays arrays;
    reader.begin_object();
    while (reader.has_next()) {
        const std::string name = reader.next_name();
        std::vector<Value>* target = nullptr;
        if (name == "conversations") target = &arrays.conversations;
        else if (name == "runs") target = &arrays.runs;
        else if (name == "tasks") target = &arrays.tasks;
        else if (name == "loops") target = &arrays.loops;
        else if (name == "messages" && with_messages) target = &arrays.messages;
        if (target == nullptr) {
            reader.skip_value();
            continue;
        }
        reader.begin_array();
        while (reader.has_next()) target->push_back(read_native_graph_value(reader));
        reader.end_array();
    }
    reader.end_object();
    return arrays;
}

}  // namespace agora::core::native_graph_import

namespace agora::core::native_graph_import {

// -----------------------------------------------------------------------------------------------
// Restored draft attachments (the restorer: importJson is ignoreUnknownKeys = true)
// ---------------------------------------------------------------------------------------------

std::optional<backup::SelectedAttachment> decode_selected_lenient(const Value& value,
                                                                  const ImportCallbacks& cb) {
    if (value.kind != Kind::Object) return std::nullopt;
    backup::SelectedAttachment attachment;
    for (const Member& member : value.members) {
        const Value& v = member.value;
        auto text = [&](std::optional<std::string>& out) {
            if (v.kind == Kind::String) out = v.text;
            else if (!(v.kind == Kind::Literal && v.text == "null")) {
                throw Exception("SerializationException", "Expected string");
            }
        };
        bool ok = true;
        if (member.key == "localId") {
            std::optional<std::string> local_id;
            text(local_id);
            attachment.local_id = local_id.value_or(cb.new_uuid());
        } else if (member.key == "uri") {
            std::optional<std::string> uri;
            text(uri);
            if (!uri.has_value()) return std::nullopt;  // required field -> kotlinx error
            attachment.uri = *uri;
        } else if (member.key == "type") {
            std::optional<std::string> type;
            text(type);
            if (!type.has_value()) return std::nullopt;
            attachment.type = *type;
        } else if (member.key == "frameCount") {
            attachment.frame_count = optional_int(v.kind == Kind::Object ? Value{} : value, "frameCount");
            ok = false;  // handled below
        } else if (member.key == "fileName") {
            text(attachment.file_name);
        } else if (member.key == "mimeType") {
            text(attachment.mime_type);
        } else if (member.key == "fileSize") {
            attachment.file_size = optional_long(value, "fileSize");
        } else if (member.key == "pixelWidth") {
            attachment.pixel_width = optional_int(value, "pixelWidth");
        } else if (member.key == "pixelHeight") {
            attachment.pixel_height = optional_int(value, "pixelHeight");
        } else if (member.key == "processedFrames") {
            if (v.kind == Kind::Array) {
                std::vector<std::string> frames;
                for (const Value& element : v.elements) {
                    if (element.kind != Kind::String) ok = false;
                    else frames.push_back(element.text);
                }
                if (ok) attachment.processed_frames = frames;
            } else if (!(v.kind == Kind::Literal && v.text == "null")) {
                ok = false;
            }
        } else if (member.key == "localPath") {
            text(attachment.local_path);
        } else if (member.key == "storage") {
            std::optional<std::string> storage;
            text(storage);
            if (storage.has_value()) {
                if (*storage == "app_private") attachment.storage = backup::AttachmentStorage::app_private;
                else if (*storage == "local_sandbox_pending") attachment.storage = backup::AttachmentStorage::local_sandbox_pending;
                else if (*storage == "local_sandbox_runtime") attachment.storage = backup::AttachmentStorage::local_sandbox_runtime;
                else return std::nullopt;
            }
        } else if (member.key == "sandboxPath") {
            text(attachment.sandbox_path);
        } else if (member.key == "importState") {
            std::optional<std::string> state;
            text(state);
            if (state.has_value()) {
                if (*state == "processing") attachment.import_state = backup::AttachmentImportState::processing;
                else if (*state == "ready") attachment.import_state = backup::AttachmentImportState::ready;
                else if (*state == "failed") attachment.import_state = backup::AttachmentImportState::failed;
                else return std::nullopt;
            }
        } else if (member.key == "preparedText") {
            text(attachment.prepared_text);
        } else if (member.key == "unavailable") {
            if (v.kind == Kind::Literal && v.text == "true") attachment.unavailable = true;
            else if (v.kind == Kind::Literal && v.text == "false") attachment.unavailable = false;
            else return std::nullopt;
        } else if (member.key == "pageCount") {
            attachment.page_count = optional_int(value, "pageCount");
        } else if (member.key == "selectedPages" || member.key == "preRenderedPaths" ||
                   member.key == "sliceIntervalMs" || member.key == "videoDurationMs") {
            // Decoded for round-trip fidelity but unused by the restore policy's outputs in
            // the frozen set; accept any shape the lenient config allows.
            continue;
        }
        // unknown keys are ignored (importJson.ignoreUnknownKeys = true)
        if (!ok) return std::nullopt;
    }
    return attachment;
}

std::optional<std::string> restore_draft_attachments(
    const std::optional<std::string>& raw, const RestoredMedia& restored,
    const ImportCallbacks& callbacks) {
    if (!raw.has_value() || json::is_blank(*raw)) return std::nullopt;
    const json::ParseResult parsed = json::parse(*raw);
    if (!parsed.value || parsed.value->kind != Kind::Array) return std::nullopt;
    std::vector<backup::SelectedAttachment> attachments;
    for (const Value& element : parsed.value->elements) {
        auto attachment = decode_selected_lenient(element, callbacks);
        if (!attachment.has_value()) return std::nullopt;
        attachments.push_back(*attachment);
    }
    auto restored_list = backup::NativeBackupMediaPolicy::restore_draft_attachments(
        attachments,
        [&](std::string_view entry) -> std::optional<std::pair<std::string, std::string>> {
            const auto it = restored.archive_files.find(std::string(entry));
            if (it == restored.archive_files.end()) return std::nullopt;
            return std::pair{it->second.absolute_path, it->second.uri};
        },
        [&](std::string_view entry) -> std::optional<std::string> {
            const auto it = restored.archive_files.find(std::string(entry));
            if (it == restored.archive_files.end()) return std::nullopt;
            return it->second.absolute_path;
        });
    if (restored_list.empty()) return std::nullopt;
    std::string out = "[";
    for (std::size_t i = 0; i < restored_list.size(); ++i) {
        if (i) out += ',';
        out += backup::encode_selected_attachment(restored_list[i]);
    }
    out += "]";
    return out;
}

// -----------------------------------------------------------------------------------------------
// Lifecycle + count + headers
// -----------------------------------------------------------------------------------------------

NativeConversationGraphImporter::NativeConversationGraphImporter(
    ::agora::core::chat_dao::ChatDao& dao, ::agora::core::semantic_index::SemanticIndexDao& semantic,
    ::agora::core::maintenance_debt::MaintenanceDebtDao& debt, ::agora::core::SqlExecutor& sql,
    ImportCallbacks callbacks)
    : dao_(dao), semantic_(semantic), debt_(debt), sql_(sql), callbacks_(std::move(callbacks)) {}

GraphCounts NativeConversationGraphImporter::count(android_json::Source& stream) {
    GraphCounts counts;
    Reader reader(stream);
    reader.begin_object();
    while (reader.has_next()) {
        const std::string name = reader.next_name();
        if (name == "conversations") counts.conversations = count_array(reader);
        else if (name == "tasks") counts.tasks = count_array(reader);
        else if (name == "loops") counts.loops = count_array(reader);
        else reader.skip_value();
    }
    reader.end_object();
    return counts;
}

ConversationGraphHeaders NativeConversationGraphImporter::read_headers(
    android_json::Source& stream, ImportStrategy strategy, const RestoredMedia& restored_media) {
    GraphArrays arrays = [&] {
        Reader reader(stream);
        return collect_graph_arrays(reader, false);
    }();

    ConversationGraphHeaders headers;

    // tasks: resolve the system prompt id, then the strategy-dependent id set.
    std::set<std::string> available_task_ids;
    if (strategy == ImportStrategy::Merge) {
        for (const std::string& id : dao_.get_all_task_ids()) available_task_ids.insert(id);
    }
    for (const Value& value : arrays.tasks) {
        TaskEntity task = decode_export_task(value);
        task.system_prompt_id =
            callbacks_.resolve_system_prompt_id
                ? callbacks_.resolve_system_prompt_id(task.system_prompt_id)
                : task.system_prompt_id;
        available_task_ids.insert(task.id);
        headers.tasks.push_back(sanitize_imported_task(task));
    }

    // conversations: restore drafts, drop unread state, clear dangling tasks.
    for (const Value& value : arrays.conversations) {
        const ExportChat exported = decode_export_chat(value);
        ChatEntity conversation;
        conversation.id = exported.id;
        conversation.title = exported.title;
        conversation.last_updated = exported.last_updated;
        conversation.is_pinned = exported.is_pinned;
        conversation.selected_branches_json = exported.selected_branches_json;
        conversation.system_prompt_id =
            callbacks_.resolve_system_prompt_id
                ? callbacks_.resolve_system_prompt_id(exported.system_prompt_id)
                : exported.system_prompt_id;
        conversation.model_id = exported.model_id;
        conversation.task_id = exported.task_id;
        conversation.origin = exported.origin;
        conversation.graduated = exported.graduated;
        conversation.draft_text = exported.draft_text;
        conversation.draft_attachments =
            restore_draft_attachments(exported.draft_attachments, restored_media, callbacks_);
        conversation.selected_run_branches_json = exported.selected_run_branches_json;
        conversation.has_unread_generation = false;
        headers.conversations.push_back(
            sanitize_imported_conversation(conversation, available_task_ids));
    }

    // LinkedHashSet: MERGE first holds the existing database ids, then the graph's.
    {
        std::set<std::string> seen;
        auto add = [&](const std::string& id) {
            if (seen.insert(id).second) headers.available_conversation_ids.push_back(id);
        };
        if (strategy == ImportStrategy::Merge) {
            for (const std::string& id : dao_.get_all_conversation_ids()) add(id);
        }
        for (const ChatEntity& conversation : headers.conversations) add(conversation.id);
    }
    auto available = [&](const std::string& conversation_id) {
        return std::find(headers.available_conversation_ids.begin(),
                         headers.available_conversation_ids.end(),
                         conversation_id) != headers.available_conversation_ids.end();
    };

    // runs: filter, uniqueness flag, terminalize, parent order.
    std::vector<ExportRun> available_raw_runs;
    for (const Value& value : arrays.runs) {
        ExportRun exported = decode_export_run(value);
        if (available(exported.conversation_id)) available_raw_runs.push_back(exported);
    }
    {
        std::set<std::string> ids;
        int duplicates = 0;
        for (const ExportRun& run : available_raw_runs) {
            if (!ids.insert(run.id).second) ++duplicates;
        }
        headers.source_run_ids_were_unique = duplicates == 0;
    }
    std::vector<native_run_archive::RunEntity> terminalized;
    terminalized.reserve(available_raw_runs.size());
    for (const ExportRun& run : available_raw_runs) {
        terminalized.push_back(native_run_archive::terminalize(to_archived_snapshot(run)));
    }
    for (const native_run_archive::RunEntity& run : native_run_archive::order_by_parent(terminalized)) {
        ::agora::core::context_compact::RunEntity converted;
        converted.id = run.id;
        converted.conversation_id = run.conversation_id;
        converted.parent_run_id = run.parent_run_id;
        // The policy enums and the runtime enums declare identical members in identical order.
        converted.status = static_cast<conversation_runtime::RunStatus>(run.status);
        converted.active_slot = run.active_slot;
        converted.started_at = run.started_at;
        converted.last_checkpoint_at = run.last_checkpoint_at;
        converted.stop_requested_at = run.stop_requested_at;
        converted.ended_at = run.ended_at;
        converted.end_reason =
            run.end_reason.has_value()
                ? std::make_optional(static_cast<conversation_runtime::RunEndReason>(*run.end_reason))
                : std::nullopt;
        converted.current_pass = run.current_pass;
        converted.legacy_ambiguous = run.legacy_ambiguous;
        headers.runs.push_back(converted);
    }

    // loops: filter by availability, sanitize.
    for (const Value& value : arrays.loops) {
        LoopEntity loop = decode_export_loop(value);
        if (!available(loop.conversation_id)) continue;
        headers.loops.push_back(sanitize_imported_loop(loop));
    }

    // conversation settings: raw graph order, no availability filter (import filters later).
    for (const Value& value : arrays.conversations) {
        const ExportChat exported = decode_export_chat(value);
        if (exported.conversation_settings.has_value()) {
            headers.conversation_settings.emplace_back(exported.id, *exported.conversation_settings);
        }
    }

    return headers;
}

}  // namespace agora::core::native_graph_import

namespace agora::core::native_graph_import {

// -----------------------------------------------------------------------------------------------
// Cross-module enum converters (each pinned module keeps its own verbatim Kotlin enum copy)
// ---------------------------------------------------------------------------------------------

legacy_run_backfill::Participant to_legacy_participant(run_recovery::Participant participant) {
    switch (participant) {
        case run_recovery::Participant::user: return legacy_run_backfill::Participant::User;
        case run_recovery::Participant::error: return legacy_run_backfill::Participant::Error;
        default: return legacy_run_backfill::Participant::Model;
    }
}

legacy_run_backfill::MessageStatus to_legacy_status(run_recovery::MessageStatus status) {
    switch (status) {
        case run_recovery::MessageStatus::success: return legacy_run_backfill::MessageStatus::Success;
        case run_recovery::MessageStatus::sending: return legacy_run_backfill::MessageStatus::Sending;
        case run_recovery::MessageStatus::thinking: return legacy_run_backfill::MessageStatus::Thinking;
        case run_recovery::MessageStatus::tool_calling:
            return legacy_run_backfill::MessageStatus::ToolCalling;
        case run_recovery::MessageStatus::transcribing:
            return legacy_run_backfill::MessageStatus::Transcribing;
        case run_recovery::MessageStatus::stopped: return legacy_run_backfill::MessageStatus::Stopped;
        default: return legacy_run_backfill::MessageStatus::Error;
    }
}

regeneration_tree_repair::Participant to_repair_participant(run_recovery::Participant participant) {
    switch (participant) {
        case run_recovery::Participant::user: return regeneration_tree_repair::Participant::User;
        case run_recovery::Participant::error: return regeneration_tree_repair::Participant::Error;
        default: return regeneration_tree_repair::Participant::Model;
    }
}

message_source::Participant to_source_participant(run_recovery::Participant participant) {
    switch (participant) {
        case run_recovery::Participant::user: return message_source::Participant::User;
        case run_recovery::Participant::error: return message_source::Participant::Error;
        default: return message_source::Participant::Model;
    }
}

::agora::core::context_compact::RunEntity to_run_entity(
    const legacy_run_backfill::PlannedLegacyRun& planned) {
    ::agora::core::context_compact::RunEntity run;
    run.id = planned.id;
    run.conversation_id = planned.conversation_id;
    run.parent_run_id = planned.parent_run_id;
    // The planner and the runtime declare the same value sets in the same order.
    run.status = static_cast<conversation_runtime::RunStatus>(planned.status);
    run.active_slot = std::nullopt;
    run.started_at = planned.started_at;
    run.last_checkpoint_at = planned.ended_at;
    run.ended_at = planned.ended_at;
    run.end_reason = std::make_optional(
        static_cast<conversation_runtime::RunEndReason>(planned.end_reason));
    run.legacy_ambiguous = planned.legacy_ambiguous;
    return run;
}

// -----------------------------------------------------------------------------------------------
// planNativeRunGraph (private in Kotlin)
// ---------------------------------------------------------------------------------------------

struct PlannedNativeRunGraph {
    std::vector<::agora::core::context_compact::RunEntity> runs;
    /** Insertion order = archived ownership (graph message) order — Kotlin's LinkedHashMap keys. */
    std::vector<legacy_run_backfill::PlannedMessageAssignment> assignments;
    std::unordered_map<std::string, std::size_t> assignment_by_message;
    std::set<std::string> recovered_run_ids;
    std::map<std::string, SelectionMap> legacy_run_selections;
    std::map<std::string, SelectionMap> message_selection_overrides;
    std::set<std::string> deleted_message_ids;
    std::map<std::string, std::string> message_parent_overrides;
};

PlannedNativeRunGraph plan_native_run_graph(android_json::Source& stream,
                                            const ConversationGraphHeaders& headers) {
    std::set<std::string> available(headers.available_conversation_ids.begin(),
                                    headers.available_conversation_ids.end());

    std::map<std::string, std::vector<legacy_run_backfill::LegacyMessageRecord>> messages_by_conversation;
    std::map<std::string, std::vector<regeneration_tree_repair::V17MessageRecord>>
        repair_messages_by_conversation;
    std::vector<native_run_archive::ArchivedMessageRunOwnership> archived_ownership;

    GraphArrays arrays = [&] {
        Reader reader(stream);
        return collect_graph_arrays(reader, true);
    }();

    for (const Value& value : arrays.messages) {
        const ExportMessage exported = decode_export_message(value);
        if (available.find(exported.conversation_id) == available.end()) continue;
        const run_recovery::Participant participant = parse_participant(exported.participant);
        const run_recovery::MessageStatus status = parse_message_status(exported.status);

        native_run_archive::ArchivedMessageRunOwnership ownership;
        ownership.message_id = exported.id;
        ownership.conversation_id = exported.conversation_id;
        ownership.run_id = exported.run_id;
        ownership.run_sequence = exported.run_sequence;
        ownership.consumed_at_pass = exported.consumed_at_pass;
        archived_ownership.push_back(ownership);

        messages_by_conversation[exported.conversation_id].push_back(
            legacy_run_backfill::LegacyMessageRecord{
                exported.id, exported.parent_id, to_legacy_participant(participant),
                to_legacy_status(status), exported.timestamp,
            });

        if (exported.run_id.has_value() && exported.run_sequence.has_value()) {
            repair_messages_by_conversation[exported.conversation_id].push_back(
                regeneration_tree_repair::V17MessageRecord{
                    exported.id,
                    exported.parent_id,
                    to_repair_participant(participant),
                    exported.timestamp,
                    *exported.run_id,
                    *exported.run_sequence,
                    participant == run_recovery::Participant::user
                        ? regeneration_tree_repair::regeneration_input_fingerprint(
                              exported.text, static_cast<int>(exported.images.size()),
                              exported.attachment_meta)
                        : std::string(),
                });
        }
    }

    PlannedNativeRunGraph planned;

    // The policy module keeps its own verbatim copy of RunEntity: convert once.
    std::vector<native_run_archive::RunEntity> policy_header_runs;
    policy_header_runs.reserve(headers.runs.size());
    for (const auto& run : headers.runs) {
        native_run_archive::RunEntity converted;
        converted.id = run.id;
        converted.conversation_id = run.conversation_id;
        converted.parent_run_id = run.parent_run_id;
        converted.status = static_cast<native_run_archive::RunStatus>(run.status);
        converted.active_slot = run.active_slot;
        converted.started_at = run.started_at;
        converted.last_checkpoint_at = run.last_checkpoint_at;
        converted.stop_requested_at = run.stop_requested_at;
        converted.ended_at = run.ended_at;
        converted.end_reason = static_cast<native_run_archive::RunEndReason>(
            run.end_reason.value_or(conversation_runtime::RunEndReason::ModelCompleted));
        converted.current_pass = run.current_pass;
        converted.legacy_ambiguous = run.legacy_ambiguous;
        policy_header_runs.push_back(converted);
    }

    const bool complete = native_run_archive::has_complete_ownership(
        policy_header_runs, archived_ownership, headers.source_run_ids_were_unique);

    if (complete) {
        std::map<std::string, std::vector<::agora::core::context_compact::RunEntity>>
            runs_by_conversation;
        std::map<std::string, ::agora::core::ChatEntity> conversations_by_id;
        for (const auto& run : headers.runs) runs_by_conversation[run.conversation_id].push_back(run);
        for (const auto& conversation : headers.conversations) {
            conversations_by_id[conversation.id] = conversation;
        }

        std::map<std::string, std::string> run_parent_updates;
        std::set<std::string> deleted_message_ids;
        std::map<std::string, std::string> message_parent_overrides;
        std::map<std::string, long long> run_sequence_overrides;
        std::map<std::string, SelectionMap> message_selection_overrides;
        std::map<std::string, SelectionMap> run_selection_overrides;

        for (const std::string& conversation_id : headers.available_conversation_ids) {
            const auto conversation_it = conversations_by_id.find(conversation_id);
            if (conversation_it == conversations_by_id.end()) continue;

            std::vector<regeneration_tree_repair::V17RunRecord> repair_runs;
            for (const auto& run : runs_by_conversation[conversation_id]) {
                repair_runs.push_back(
                    regeneration_tree_repair::V17RunRecord{run.id, run.parent_run_id,
                                                            run.started_at});
            }
            const SelectionMap message_selections =
                decode_stored_selections(conversation_it->second.selected_branches_json)
                    .value_or(SelectionMap{});
            const SelectionMap run_selections =
                decode_stored_selections(conversation_it->second.selected_run_branches_json)
                    .value_or(SelectionMap{});

            const regeneration_tree_repair::Plan repair =
                regeneration_tree_repair::plan(repair_runs,
                                               repair_messages_by_conversation[conversation_id],
                                               message_selections, run_selections);
            if (repair.inferred_run_ids.empty()) continue;

            for (const auto& [run_id, parent_id] : repair.run_parent_updates) {
                run_parent_updates[run_id] = parent_id;
            }
            deleted_message_ids.insert(repair.deleted_message_ids.begin(),
                                       repair.deleted_message_ids.end());
            for (const auto& [message_id, parent_id] : repair.message_parent_updates) {
                message_parent_overrides[message_id] = parent_id;
            }
            for (const auto& [message_id, sequence] : repair.run_sequence_updates) {
                run_sequence_overrides[message_id] = sequence;
            }
            message_selection_overrides[conversation_id] = repair.message_selections;
            run_selection_overrides[conversation_id] = repair.run_selections;
        }

        std::vector<native_run_archive::RunEntity> repaired_policy_runs;
        for (const auto& run : headers.runs) {
            const auto update = run_parent_updates.find(run.id);
            if (update != run_parent_updates.end()) {
                native_run_archive::RunEntity copy;
                copy.id = run.id;
                copy.conversation_id = run.conversation_id;
                copy.parent_run_id = update->second;
                copy.status = static_cast<native_run_archive::RunStatus>(run.status);
                copy.active_slot = run.active_slot;
                copy.started_at = run.started_at;
                copy.last_checkpoint_at = run.last_checkpoint_at;
                copy.stop_requested_at = run.stop_requested_at;
                copy.ended_at = run.ended_at;
                copy.end_reason = static_cast<native_run_archive::RunEndReason>(
                    run.end_reason.value_or(conversation_runtime::RunEndReason::ModelCompleted));
                copy.current_pass = run.current_pass;
                copy.legacy_ambiguous = true;
                repaired_policy_runs.push_back(copy);
            } else {
                native_run_archive::RunEntity copy;
                copy.id = run.id;
                copy.conversation_id = run.conversation_id;
                copy.parent_run_id = run.parent_run_id;
                copy.status = static_cast<native_run_archive::RunStatus>(run.status);
                copy.active_slot = run.active_slot;
                copy.started_at = run.started_at;
                copy.last_checkpoint_at = run.last_checkpoint_at;
                copy.stop_requested_at = run.stop_requested_at;
                copy.ended_at = run.ended_at;
                copy.end_reason = static_cast<native_run_archive::RunEndReason>(
                    run.end_reason.value_or(conversation_runtime::RunEndReason::ModelCompleted));
                copy.current_pass = run.current_pass;
                copy.legacy_ambiguous = run.legacy_ambiguous;
                repaired_policy_runs.push_back(copy);
            }
        }
        for (const native_run_archive::RunEntity& run :
             native_run_archive::order_by_parent(repaired_policy_runs)) {
            ::agora::core::context_compact::RunEntity converted;
            converted.id = run.id;
            converted.conversation_id = run.conversation_id;
            converted.parent_run_id = run.parent_run_id;
            converted.status = static_cast<conversation_runtime::RunStatus>(run.status);
            converted.active_slot = run.active_slot;
            converted.started_at = run.started_at;
            converted.last_checkpoint_at = run.last_checkpoint_at;
            converted.stop_requested_at = run.stop_requested_at;
            converted.ended_at = run.ended_at;
            converted.end_reason = run.end_reason.has_value()
                                       ? std::make_optional(static_cast<conversation_runtime::RunEndReason>(*run.end_reason))
                                       : std::nullopt;
            converted.current_pass = run.current_pass;
            converted.legacy_ambiguous = run.legacy_ambiguous;
            planned.runs.push_back(converted);
        }

        for (const auto& ownership : archived_ownership) {
            if (deleted_message_ids.find(ownership.message_id) != deleted_message_ids.end()) {
                continue;
            }
            if (!ownership.run_id.has_value()) {
                throw Exception("IllegalStateException", "Required value was null.");
            }
            long long sequence = 0;
            const auto override = run_sequence_overrides.find(ownership.message_id);
            if (override != run_sequence_overrides.end()) sequence = override->second;
            else if (ownership.run_sequence.has_value()) sequence = *ownership.run_sequence;
            else throw Exception("IllegalStateException", "Required value was null.");

            legacy_run_backfill::PlannedMessageAssignment assignment;
            assignment.message_id = ownership.message_id;
            assignment.run_id = *ownership.run_id;
            assignment.run_sequence = sequence;
            assignment.consumed_at_pass = ownership.consumed_at_pass;
            planned.assignment_by_message[assignment.message_id] = planned.assignments.size();
            planned.assignments.push_back(assignment);
        }

        for (const auto& run : planned.runs) {
            if (run.end_reason ==
                std::make_optional(conversation_runtime::RunEndReason::ProcessRecovered)) {
                planned.recovered_run_ids.insert(run.id);
            }
        }
        planned.legacy_run_selections = run_selection_overrides;
        planned.message_selection_overrides = message_selection_overrides;
        planned.deleted_message_ids = deleted_message_ids;
        planned.message_parent_overrides = message_parent_overrides;
        return planned;
    }

    // Legacy backfill branch: no usable ownership — derive runs, assignments and selections.
    std::vector<native_run_archive::RunEntity> policy_runs;
    for (const auto& conversation : headers.conversations) {
        const auto messages_it = messages_by_conversation.find(conversation.id);
        static const std::vector<legacy_run_backfill::LegacyMessageRecord> kNoMessages;
        const std::vector<legacy_run_backfill::LegacyMessageRecord>& messages =
            messages_it != messages_by_conversation.end() ? messages_it->second : kNoMessages;
        const legacy_run_backfill::PlanResult result =
            legacy_run_backfill::plan(conversation.id, messages);
        if (!result.ok) throw Exception("IllegalArgumentException", result.error);
        for (const auto& planned_run : result.plan.runs) {
            native_run_archive::RunEntity run;
            run.id = planned_run.id;
            run.conversation_id = planned_run.conversation_id;
            run.parent_run_id = planned_run.parent_run_id;
            run.status = static_cast<native_run_archive::RunStatus>(planned_run.status);
            run.active_slot = std::nullopt;
            run.started_at = planned_run.started_at;
            run.last_checkpoint_at = planned_run.ended_at;
            run.ended_at = planned_run.ended_at;
            run.end_reason =
                static_cast<native_run_archive::RunEndReason>(planned_run.end_reason);
            run.legacy_ambiguous = planned_run.legacy_ambiguous;
            policy_runs.push_back(run);
        }
        for (const auto& assignment : result.plan.assignments) {
            planned.assignment_by_message[assignment.message_id] = planned.assignments.size();
            planned.assignments.push_back(assignment);
        }

        const SelectionMap message_selections =
            decode_stored_selections(conversation.selected_branches_json)
                .value_or(SelectionMap{});
        const legacy_run_backfill::SelectionResult selections =
            legacy_run_backfill::selected_run_branches(messages, result.plan, message_selections);
        if (!selections.ok) throw Exception("IllegalArgumentException", selections.error);
        planned.legacy_run_selections[conversation.id] = selections.selections;
    }
    for (const native_run_archive::RunEntity& run :
         native_run_archive::order_by_parent(policy_runs)) {
        ::agora::core::context_compact::RunEntity converted;
        converted.id = run.id;
        converted.conversation_id = run.conversation_id;
        converted.parent_run_id = run.parent_run_id;
        converted.status = static_cast<conversation_runtime::RunStatus>(run.status);
        converted.active_slot = run.active_slot;
        converted.started_at = run.started_at;
        converted.last_checkpoint_at = run.last_checkpoint_at;
        converted.stop_requested_at = run.stop_requested_at;
        converted.ended_at = run.ended_at;
        converted.end_reason = run.end_reason.has_value()
                                   ? std::make_optional(static_cast<conversation_runtime::RunEndReason>(*run.end_reason))
                                   : std::nullopt;
        converted.current_pass = run.current_pass;
        converted.legacy_ambiguous = run.legacy_ambiguous;
        planned.runs.push_back(converted);
    }
    return planned;
}

}  // namespace agora::core::native_graph_import

namespace agora::core::native_graph_import {

// -----------------------------------------------------------------------------------------------
// ExportMessageEntity.toMessageEntity
// -----------------------------------------------------------------------------------------------

::agora::core::MessageEntity to_message_entity(
    const ExportMessage& exported, const RestoredMedia& restored,
    const legacy_run_backfill::PlannedMessageAssignment& assignment,
    const std::set<std::string>& recovered_run_ids, int archive_version) {
    const run_recovery::Participant participant = parse_participant(exported.participant);
    run_recovery::MessageStatus status = parse_message_status(exported.status);

    std::map<int, int> old_to_new_image_index;
    std::vector<std::string> restored_images;
    if (archive_version >= 4) {
        for (std::size_t i = 0; i < exported.images.size(); ++i) {
            const auto it = restored.archive_files.find(exported.images[i]);
            if (it != restored.archive_files.end()) {
                old_to_new_image_index[static_cast<int>(i)] =
                    static_cast<int>(restored_images.size());
                restored_images.push_back(it->second.uri);
            }
        }
    } else {
        const auto images_it = restored.legacy_images_by_message.find(exported.id);
        if (images_it != restored.legacy_images_by_message.end()) {
            // Kotlin: toSortedMap() — ordered by the numeric index.
            for (const auto& [old_index, uri] : images_it->second) {
                old_to_new_image_index[old_index] = static_cast<int>(restored_images.size());
                restored_images.push_back(uri);
            }
        }
    }

    if (recovered_run_ids.find(assignment.run_id) != recovered_run_ids.end() &&
        participant == run_recovery::Participant::model &&
        (status == run_recovery::MessageStatus::sending ||
         status == run_recovery::MessageStatus::thinking ||
         status == run_recovery::MessageStatus::tool_calling ||
         status == run_recovery::MessageStatus::transcribing)) {
        status = run_recovery::MessageStatus::stopped;
    }

    std::map<int, std::string> legacy_video_uris;
    if (archive_version < 4) {
        const auto videos_it = restored.legacy_videos_by_message.find(exported.id);
        if (videos_it != restored.legacy_videos_by_message.end()) {
            legacy_video_uris = videos_it->second;
        }
    }

    ::agora::core::MessageEntity message;
    message.id = exported.id;
    message.conversation_id = exported.conversation_id;
    message.parent_id = exported.parent_id;
    message.text = exported.text;
    message.images = restored_images;
    message.thoughts = exported.thoughts;
    message.thought_title = exported.thought_title;
    message.token_count = exported.token_count;
    message.input_token_count = exported.input_token_count;
    message.cached_input_token_count = exported.cached_input_token_count;
    message.cache_write_input_token_count = exported.cache_write_input_token_count;
    message.uncached_input_token_count = exported.uncached_input_token_count;
    message.output_token_count = exported.output_token_count;
    message.reasoning_token_count = exported.reasoning_token_count;
    message.generation_duration_ms =
        exported.generation_duration_ms.has_value() && *exported.generation_duration_ms > 0
            ? exported.generation_duration_ms
            : std::nullopt;
    message.status = status;
    message.participant = participant;
    message.timestamp = exported.timestamp;
    message.thought_time_ms = exported.thought_time_ms;
    message.model_name = exported.model_name;
    message.tool_call_json = backup::NativeBackupMediaPolicy::restore_tool_image_paths(
        exported.tool_call_json, archive_version,
        [&](std::string_view entry) -> std::optional<std::string> {
            const auto it = restored.archive_files.find(std::string(entry));
            if (it == restored.archive_files.end()) return std::nullopt;
            return it->second.absolute_path;
        });
    message.attachment_meta = backup::NativeBackupMediaPolicy::restore_attachment_meta(
        exported.attachment_meta, archive_version, old_to_new_image_index, legacy_video_uris,
        [&](std::string_view entry) -> std::optional<std::string> {
            const auto it = restored.archive_files.find(std::string(entry));
            if (it == restored.archive_files.end()) return std::nullopt;
            return it->second.uri;
        });
    message.run_id = assignment.run_id;
    message.run_sequence = assignment.run_sequence;
    message.consumed_at_pass = assignment.consumed_at_pass;
    message.source_json =
        message_source::sanitize_imported(exported.source_json, to_source_participant(participant));
    return message;
}
// -----------------------------------------------------------------------------------------------
// importConversationGraph
// ---------------------------------------------------------------------------------------------

std::string encode_settings_map(
    const std::vector<std::pair<std::string, export_extra_settings::ConversationSettings>>&
        settings_map) {
    std::string out = "{";
    bool first = true;
    for (const auto& [id, settings] : settings_map) {
        if (!first) out += ',';
        first = false;
        out += '"';
        json::escape_to(out, id);
        out += "\":{";
        bool field_first = true;
        auto put_int = [&](std::string_view key, const std::optional<int>& value) {
            if (!value.has_value()) return;
            if (!field_first) out += ',';
            field_first = false;
            out += '"';
            json::escape_to(out, key);
            out += "\":";
            out += std::to_string(*value);
        };
        auto put_float = [&](std::string_view key, const std::optional<float>& value) {
            if (!value.has_value()) return;
            if (!field_first) out += ',';
            field_first = false;
            out += '"';
            json::escape_to(out, key);
            out += "\":";
            out += openai::format_kotlin_float(*value);  // kotlinx Float.toString
        };
        auto put_bool = [&](std::string_view key, const std::optional<bool>& value) {
            if (!value.has_value()) return;
            if (!field_first) out += ',';
            field_first = false;
            out += '"';
            json::escape_to(out, key);
            out += "\":";
            out += *value ? "true" : "false";
        };
        auto put_text = [&](std::string_view key, const std::optional<std::string>& value) {
            if (!value.has_value()) return;
            if (!field_first) out += ',';
            field_first = false;
            out += '"';
            json::escape_to(out, key);
            out += "\":\"";
            json::escape_to(out, *value);
            out += '"';
        };
        put_int("contextWindow", settings.context_window);
        put_float("temperature", settings.temperature);
        put_int("maxTokens", settings.max_tokens);
        put_float("topP", settings.top_p);
        put_float("frequencyPenalty", settings.frequency_penalty);
        put_float("presencePenalty", settings.presence_penalty);
        put_bool("codeExecutionEnabled", settings.code_execution_enabled);
        put_bool("googleSearchEnabled", settings.google_search_enabled);
        put_bool("openAiWebSearchEnabled", settings.open_ai_web_search_enabled);
        put_bool("thinkingEnabled", settings.thinking_enabled);
        put_text("thinkingLevel", settings.thinking_level);
        put_bool("thinkingBudgetEnabled", settings.thinking_budget_enabled);
        put_int("thinkingBudgetTokens", settings.thinking_budget_tokens);
        put_bool("openAiServiceTierEnabled", settings.open_ai_service_tier_enabled);
        put_text("openAiServiceTier", settings.open_ai_service_tier);
        put_bool("webSearchEnabled", settings.web_search_enabled);
        put_bool("shellEnabled", settings.shell_enabled);
        put_bool("lowContextModeEnabled", settings.low_context_mode_enabled);
        out += '}';
    }
    out += '}';
    return out;
}

std::string NativeConversationGraphImporter::import_graph(
    native_graph::NativeConversationGraphSource& source, ImportStrategy strategy,
    const ConversationGraphHeaders& headers, const RestoredMedia& restored_media,
    int archive_version, const ::agora::core::semantic_index::SemanticModelSnapshot& snapshot) {
    std::set<std::string> imported_ids(headers.available_conversation_ids.begin(),
                                       headers.available_conversation_ids.end());
    std::vector<std::pair<std::string, export_extra_settings::ConversationSettings>>
        imported_settings;
    for (const auto& [id, settings] : headers.conversation_settings) {
        if (imported_ids.find(id) != imported_ids.end()) imported_settings.emplace_back(id, settings);
    }
    const std::string settings_json = encode_settings_map(imported_settings);
    const std::string mode = strategy == ImportStrategy::Replace ? "REPLACE" : "MERGE";

    // planNativeRunGraph over a fresh chained stream (Kotlin: graphSource.open().use { ... }).
    PlannedNativeRunGraph planned = [&] {
        native_graph::NativeConversationGraphSource::Stream stream = source.open_stream();
        GraphStreamAdapter adapter(stream);
        return plan_native_run_graph(adapter, headers);
    }();

    const std::string transfer_id = callbacks_.new_uuid();
    const std::int64_t at = callbacks_.now_millis();

    // withSemanticGraphMutation: one RoomDatabase transaction level; the block runs first and
    // the semantic deletions/reconcile follow, exactly like the verbatim extension.
    sql_.begin();
    try {
        if (strategy == ImportStrategy::Replace) {
            dao_.delete_all_loops();
            dao_.delete_all_conversations();
            dao_.delete_all_tasks();
        }
        for (const TaskEntity& task : headers.tasks) dao_.upsert_task(task);

        for (const ChatEntity& conversation : headers.conversations) {
            ChatEntity written = conversation;
            if (const auto it = planned.message_selection_overrides.find(conversation.id);
                it != planned.message_selection_overrides.end()) {
                written.selected_branches_json = encode_stored_selections(it->second);
            }
            if (const auto it = planned.legacy_run_selections.find(conversation.id);
                it != planned.legacy_run_selections.end()) {
                written.selected_run_branches_json = encode_stored_selections(it->second);
            }
            dao_.upsert_conversation(written);
        }

        for (const auto& run : planned.runs) {
            if (!dao_.get_run(run.id)) dao_.insert_run(run);
        }

        // importMessagesFromGraph: 64-row batches, MERGE skips existing ids unless images.
        {
            native_graph::NativeConversationGraphSource::Stream stream = source.open_stream();
            GraphStreamAdapter adapter(stream);
            GraphArrays arrays = [&] {
                Reader reader(adapter);
                return collect_graph_arrays(reader, true);
            }();
            std::vector<::agora::core::MessageEntity> batch;
            const auto flush_batch = [&] {
                if (batch.empty()) return;
                std::set<std::string> existing;
                if (strategy == ImportStrategy::Merge) {
                    std::vector<std::string> ids;
                    ids.reserve(batch.size());
                    for (const auto& message : batch) ids.push_back(message.id);
                    for (const std::string& id : dao_.find_existing_message_ids(ids)) {
                        existing.insert(id);
                    }
                }
                for (const auto& message : batch) {
                    const bool known = existing.find(message.id) != existing.end();
                    if (!known || !message.images.empty()) dao_.upsert_message(message);
                }
                batch.clear();
            };
            for (const Value& value : arrays.messages) {
                const ExportMessage exported = decode_export_message(value);
                if (imported_ids.find(exported.conversation_id) == imported_ids.end()) {
                    continue;
                }
                if (planned.deleted_message_ids.find(exported.id) !=
                    planned.deleted_message_ids.end()) {
                    continue;
                }
                const auto assignment_it = planned.assignment_by_message.find(exported.id);
                if (assignment_it == planned.assignment_by_message.end()) {
                    throw Exception("IllegalStateException",
                                    "Message " + exported.id + " has no planned Run assignment");
                }
                ::agora::core::MessageEntity message = to_message_entity(
                    exported, restored_media, planned.assignments[assignment_it->second],
                    planned.recovered_run_ids, archive_version);
                if (const auto parent_it = planned.message_parent_overrides.find(exported.id);
                    parent_it != planned.message_parent_overrides.end()) {
                    message.parent_id = parent_it->second;
                }
                batch.push_back(message);
                if (batch.size() >= 64) flush_batch();
            }
            flush_batch();
        }

        for (const LoopEntity& loop : headers.loops) dao_.upsert_loop(loop, at);

        for (const auto& [conversation_id, settings] : imported_settings) {
            (void)settings;
            dao_.delete_conversation_settings_transfer(conversation_id);
        }
        dao_.upsert_conversation_settings_import_transfer(
            ::agora::core::chat_dao::ConversationSettingsImportTransferEntity{
                0, transfer_id, settings_json, mode});

        // Semantic mutation tail: block already ran; now clear and request the reconcile.
        if (strategy == ImportStrategy::Replace) {
            semantic_.delete_all_embeddings();
        } else {
            std::vector<std::string> clear_ids;
            clear_ids.reserve(planned.assignments.size());
            for (const auto& assignment : planned.assignments) {
                if (!assignment.message_id.empty()) clear_ids.push_back(assignment.message_id);
            }
            if (!clear_ids.empty()) semantic_.delete_embeddings_for_messages(clear_ids);
        }
        semantic_index::request_semantic_reconcile(semantic_, snapshot, at);

        // The maintenance enqueues ride the same transaction (Kotlin order).
        for (const std::string_view kind : kReconcileKinds) {
            debt_.enqueue(kind, kReconcileIdentity, at);
        }

        sql_.commit();
    } catch (...) {
        sql_.rollback();
        throw;
    }

    callbacks_.schedule_maintenance();
    return transfer_id;
}

}  // namespace agora::core::native_graph_import
