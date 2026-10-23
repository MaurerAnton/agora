// Golden parity tests for data/local/ChatDao.kt and data/ConversationExportSnapshotReader.kt.
//
// The stands compile the app's DAO interfaces and snapshot reader verbatim over a real SQLite
// database with Room's generated adapter and reader-connection surface replaced. These tests
// drive the C++ ports over the same Room schema and compare every step result and the final table
// dump (or key inventory) with the frozen Kotlin traces.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <functional>
#include <fstream>
#include <map>
#include <optional>
#include <set>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "agora/core/chat_dao.hpp"
#include "agora/core/conversation_export_snapshot_reader.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace automation = agora::core::automation;
namespace chat = agora::core::chat_dao;
namespace compact = agora::core::context_compact;
namespace export_snapshot = agora::core::conversation_export_snapshot;
namespace json = agora::core::json;
namespace new_chat = agora::core::new_chat_persist;
namespace recovery = agora::core::run_recovery;
namespace runtime = agora::core::conversation_runtime;

using agora::core::ChatEntity;
using agora::core::KotlinError;
using agora::core::MessageEntity;
using agora::core::SqlArgs;
using agora::core::SqlConstraintError;
using agora::core::SqlError;
using agora::core::SqlExecutor;
using agora::core::SqlRow;
using agora::core::SqlValue;
using chat::ChatConversation;
using chat::ChatDao;
using chat::ConversationSettingsImportTransferEntity;
using chat::ConversationSettingsTransferEntity;
using chat::DrawerConversationRow;
using chat::EmbeddingEntity;
using chat::ExecutionMessageSummaryRow;
using chat::MessageStreamCheckpoint;
using chat::RunGraphCommit;
using chat::SelectionMap;
using chat::ToolRoundCommit;
using compact::RunEntity;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null_element(const json::Value& value) {
    return value.kind == json::Kind::Literal && value.text == "null";
}

std::string primitive_content(const json::Value& value) {
    if (value.kind == json::Kind::String) return value.text;
    if (value.kind == json::Kind::Literal) return value.text;
    scenario_fail("expected a JSON primitive");
}

std::string required_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Array || value->kind == json::Kind::Object) {
        throw KotlinError("IllegalStateException", "missing string " + std::string(name));
    }
    return primitive_content(*value);
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_null_element(*value)) return std::nullopt;
    return primitive_content(*value);
}

std::int64_t required_long(const json::Value& object, std::string_view name) {
    const std::string content = required_string(object, name);
    std::size_t consumed = 0;
    try {
        const long long parsed = std::stoll(content, &consumed);
        if (consumed != content.size()) throw std::invalid_argument("trailing");
        return parsed;
    } catch (const std::exception&) {
        throw KotlinError("NumberFormatException", "For input string: \"" + content + "\"");
    }
}

std::optional<std::int64_t> optional_long(const json::Value& object, std::string_view name) {
    const std::optional<std::string> content = optional_string(object, name);
    if (!content.has_value()) return std::nullopt;
    std::size_t consumed = 0;
    try {
        const long long parsed = std::stoll(*content, &consumed);
        if (consumed != content->size()) throw std::invalid_argument("trailing");
        return parsed;
    } catch (const std::exception&) {
        throw KotlinError("NumberFormatException", "For input string: \"" + *content + "\"");
    }
}

int required_int(const json::Value& object, std::string_view name) {
    return static_cast<int>(required_long(object, name));
}

std::optional<int> optional_int(const json::Value& object, std::string_view name) {
    const std::optional<std::int64_t> value = optional_long(object, name);
    if (!value.has_value()) return std::nullopt;
    return static_cast<int>(*value);
}

bool required_bool(const json::Value& object, std::string_view name) {
    const std::string content = required_string(object, name);
    if (content == "true") return true;
    if (content == "false") return false;
    throw KotlinError("IllegalStateException", "expected a boolean");
}

std::optional<bool> optional_bool(const json::Value& object, std::string_view name) {
    const std::optional<std::string> content = optional_string(object, name);
    if (!content.has_value()) return std::nullopt;
    if (*content == "true") return true;
    if (*content == "false") return false;
    throw KotlinError("IllegalStateException", "expected a boolean");
}

std::vector<std::string> string_list(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != json::Kind::Array) {
        throw KotlinError("IllegalStateException", "expected a string array");
    }
    std::vector<std::string> out;
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::String) {
            throw KotlinError("IllegalStateException", "expected a string element");
        }
        out.push_back(element.text);
    }
    return out;
}

const json::Value& required_object(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Object) {
        throw KotlinError("IllegalStateException", "missing object " + std::string(name));
    }
    return *value;
}

const json::Value* optional_object(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind == json::Kind::Literal) return nullptr;
    if (value->kind != json::Kind::Object) {
        throw KotlinError("IllegalStateException", "expected object " + std::string(name));
    }
    return value;
}

std::vector<json::Value> objects(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != json::Kind::Array) {
        throw KotlinError("IllegalStateException", "expected an object array");
    }
    std::vector<json::Value> out;
    for (const json::Value& element : value->elements) out.push_back(element);
    return out;
}

std::vector<std::pair<std::string, std::string>> string_map(const json::Value& object,
                                                            std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != json::Kind::Object) {
        throw KotlinError("IllegalStateException", "expected an object map");
    }
    std::vector<std::pair<std::string, std::string>> out;
    for (const json::Member& member : value->members) {
        if (member.value.kind != json::Kind::String) {
            throw KotlinError("IllegalStateException", "expected a string map value");
        }
        out.emplace_back(member.key, member.value.text);
    }
    return out;
}

SelectionMap selection_map(const json::Value& object, std::string_view name) {
    SelectionMap out;
    for (const auto& [key, value] : string_map(object, name)) {
        out.emplace_back(key == "null" ? std::nullopt : std::optional<std::string>(key), value);
    }
    return out;
}

// ---- json builders ------------------------------------------------------------------------

json::Value json_null() {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = "null";
    return value;
}

json::Value json_bool(bool flag) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = flag ? "true" : "false";
    return value;
}

json::Value json_string(std::string_view text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::string(text);
    return value;
}

json::Value json_long(std::int64_t number) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::to_string(number);
    return value;
}

json::Value json_array(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value json_object(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        json::Member entry;
        entry.key = std::move(member.first);
        entry.value = std::move(member.second);
        value.members.push_back(std::move(entry));
    }
    return value;
}

json::Value optional_json_string(const std::optional<std::string>& text) {
    if (!text.has_value()) return json_null();
    return json_string(*text);
}

json::Value optional_json_long(const std::optional<std::int64_t>& value) {
    if (!value.has_value()) return json_null();
    return json_long(*value);
}

json::Value optional_json_int(const std::optional<int>& value) {
    if (!value.has_value()) return json_null();
    return json_long(*value);
}

json::Value string_array(const std::vector<std::string>& values) {
    std::vector<json::Value> elements;
    elements.reserve(values.size());
    for (const std::string& value : values) elements.push_back(json_string(value));
    return json_array(std::move(elements));
}

bool json_equal(const json::Value& left, const json::Value& right) {
    if (left.kind != right.kind) return false;
    if (left.kind == json::Kind::String || left.kind == json::Kind::Literal) {
        return left.text == right.text;
    }
    if (left.kind == json::Kind::Array) {
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

// ---- base64 -------------------------------------------------------------------------------

constexpr char kBase64[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

std::string base64_encode(const std::vector<std::uint8_t>& bytes) {
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    for (std::size_t i = 0; i < bytes.size(); i += 3) {
        const std::uint32_t a = bytes[i];
        const std::uint32_t b = i + 1 < bytes.size() ? bytes[i + 1] : 0;
        const std::uint32_t c = i + 2 < bytes.size() ? bytes[i + 2] : 0;
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
        if (ch == '=' || ch == '\n' || ch == '\r' || ch == ' ' || ch == '\t') continue;
        const char* found = std::find(std::begin(kBase64), std::end(kBase64) - 1, ch);
        if (found == std::end(kBase64) - 1) scenario_fail("invalid base64");
        accumulator = (accumulator << 6) | static_cast<int>(found - std::begin(kBase64));
        bits += 6;
        if (bits >= 8) {
            bits -= 8;
            out.push_back(static_cast<std::uint8_t>((accumulator >> bits) & 0xFF));
        }
    }
    return out;
}

// ---- sqlite executor ----------------------------------------------------------------------

class SqliteExecutor : public agora::core::SqlExecutor {
public:
    explicit SqliteExecutor(const std::filesystem::path& schema_file) {
        if (sqlite3_open(":memory:", &db_) != SQLITE_OK) {
            scenario_fail(std::string("cannot open sqlite: ") + sqlite3_errmsg(db_));
        }
        exec_raw("PRAGMA foreign_keys = ON");
        const std::string schema = read_file(schema_file);
        char* error = nullptr;
        if (sqlite3_exec(db_, schema.c_str(), nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "schema failed";
            sqlite3_free(error);
            scenario_fail("schema: " + message);
        }
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
        if ((rc & 0xFF) == SQLITE_CONSTRAINT) throw SqlConstraintError(message);
        throw SqlError("sqlite: " + message + " in " + sql);
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
            throw SqlError("sqlite: " + message + " in " + sql);
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
            scenario_fail("sqlite: " + message + " in " + sql);
        }
    }

    sqlite3_stmt* prepare(const std::string& sql) {
        sqlite3_stmt* statement = nullptr;
        const int rc = sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr);
        if (rc != SQLITE_OK) {
            throw SqlError(std::string("sqlite prepare: ") + sqlite3_errmsg(db_) + " in " + sql);
        }
        return statement;
    }

    void bind(sqlite3_stmt* statement, const SqlArgs& args) {
        for (std::size_t i = 0; i < args.size(); ++i) {
            const int index = static_cast<int>(i + 1);
            const SqlValue& value = args[i];
            int rc = SQLITE_OK;
            switch (value.kind) {
                case SqlValue::Kind::Null:
                    rc = sqlite3_bind_null(statement, index);
                    break;
                case SqlValue::Kind::Text:
                    rc = sqlite3_bind_text(statement, index, value.text.data(),
                                           static_cast<int>(value.text.size()), SQLITE_TRANSIENT);
                    break;
                case SqlValue::Kind::Integer:
                    rc = sqlite3_bind_int64(statement, index, value.integer);
                    break;
                case SqlValue::Kind::Real:
                    rc = sqlite3_bind_double(statement, index, value.real);
                    break;
                case SqlValue::Kind::Blob: {
                    const void* data = value.blob.empty()
                                           ? static_cast<const void*>("")
                                           : static_cast<const void*>(value.blob.data());
                    rc = sqlite3_bind_blob(statement, index, data,
                                           static_cast<int>(value.blob.size()), SQLITE_TRANSIENT);
                    break;
                }
            }
            if (rc != SQLITE_OK) throw SqlError(std::string("sqlite bind: ") + sqlite3_errmsg(db_));
        }
    }

    static SqlRow read_row(sqlite3_stmt* statement) {
        SqlRow row;
        const int columns = sqlite3_column_count(statement);
        for (int i = 0; i < columns; ++i) {
            SqlValue value;
            switch (sqlite3_column_type(statement, i)) {
                case SQLITE_NULL:
                    value.kind = SqlValue::Kind::Null;
                    break;
                case SQLITE_INTEGER:
                    value.kind = SqlValue::Kind::Integer;
                    value.integer = sqlite3_column_int64(statement, i);
                    break;
                case SQLITE_FLOAT:
                    value.kind = SqlValue::Kind::Real;
                    value.real = sqlite3_column_double(statement, i);
                    break;
                case SQLITE_TEXT: {
                    value.kind = SqlValue::Kind::Text;
                    const auto* text = sqlite3_column_text(statement, i);
                    const int length = sqlite3_column_bytes(statement, i);
                    value.text.assign(reinterpret_cast<const char*>(text),
                                      static_cast<std::size_t>(length));
                    break;
                }
                case SQLITE_BLOB: {
                    value.kind = SqlValue::Kind::Blob;
                    const auto* data = static_cast<const std::uint8_t*>(
                        sqlite3_column_blob(statement, i));
                    const int length = sqlite3_column_bytes(statement, i);
                    value.blob.assign(data, data + length);
                    break;
                }
            }
            row.push_back(std::move(value));
        }
        return row;
    }

    sqlite3* db_ = nullptr;
};

// ---- entity construction ------------------------------------------------------------------

ChatEntity conversation(const json::Value& step) {
    ChatEntity entity;
    entity.id = required_string(step, "id");
    entity.title = optional_string(step, "title").value_or("");
    entity.last_updated = optional_long(step, "lastUpdated").value_or(0);
    entity.data_changed_at = optional_long(step, "dataChangedAt").value_or(0);
    entity.selected_branches_json = optional_string(step, "selectedBranchesJson");
    entity.system_prompt_id = optional_string(step, "systemPromptId");
    entity.model_id = optional_string(step, "modelId");
    entity.task_id = optional_string(step, "taskId");
    entity.origin = optional_string(step, "origin").value_or("user");
    entity.graduated = optional_bool(step, "graduated").value_or(false);
    entity.draft_text = optional_string(step, "draftText").value_or("");
    entity.draft_attachments = optional_string(step, "draftAttachments");
    entity.selected_run_branches_json = optional_string(step, "selectedRunBranchesJson");
    entity.has_unread_generation = optional_bool(step, "hasUnreadGeneration").value_or(false);
    entity.is_pinned = optional_bool(step, "isPinned").value_or(false);
    return entity;
}

RunEntity run(const json::Value& step) {
    RunEntity entity;
    entity.id = required_string(step, "id");
    entity.conversation_id = required_string(step, "conversationId");
    entity.parent_run_id = optional_string(step, "parentRunId");
    const std::string status = optional_string(step, "status").value_or("ACTIVE");
    if (status == "ACTIVE") entity.status = runtime::RunStatus::Active;
    else if (status == "STOPPING") entity.status = runtime::RunStatus::Stopping;
    else if (status == "COMPLETED") entity.status = runtime::RunStatus::Completed;
    else if (status == "STOPPED") entity.status = runtime::RunStatus::Stopped;
    else if (status == "FAILED") entity.status = runtime::RunStatus::Failed;
    else throw KotlinError("IllegalArgumentException", "Unknown RunStatus " + status);
    entity.active_slot = field(step, "activeSlot") != nullptr ? optional_long(step, "activeSlot")
                                                               : std::optional<std::int64_t>(1);
    entity.started_at = optional_long(step, "startedAt").value_or(0);
    entity.last_checkpoint_at = optional_long(step, "lastCheckpointAt").value_or(0);
    entity.stop_requested_at = optional_long(step, "stopRequestedAt");
    entity.ended_at = optional_long(step, "endedAt");
    const std::optional<std::string> reason = optional_string(step, "endReason");
    if (reason.has_value()) {
        if (*reason == "MODEL_COMPLETED") entity.end_reason = runtime::RunEndReason::ModelCompleted;
        else if (*reason == "USER_STOPPED") entity.end_reason = runtime::RunEndReason::UserStopped;
        else if (*reason == "PROCESS_RECOVERED") entity.end_reason = runtime::RunEndReason::ProcessRecovered;
        else if (*reason == "PROVIDER_ERROR") entity.end_reason = runtime::RunEndReason::ProviderError;
        else throw KotlinError("IllegalArgumentException", "Unknown RunEndReason " + *reason);
    }
    entity.current_pass = optional_int(step, "currentPass").value_or(0);
    entity.legacy_ambiguous = optional_bool(step, "legacyAmbiguous").value_or(false);
    return entity;
}

MessageEntity message(const json::Value& step) {
    MessageEntity entity;
    entity.id = required_string(step, "id");
    entity.conversation_id = required_string(step, "conversationId");
    entity.parent_id = optional_string(step, "parentId");
    entity.text = optional_string(step, "text").value_or("");
    entity.images = string_list(step, "images");
    entity.thoughts = optional_string(step, "thoughts");
    entity.thought_title = optional_string(step, "thoughtTitle");
    entity.token_count = optional_int(step, "tokenCount").value_or(0);
    entity.input_token_count = optional_int(step, "inputTokenCount");
    entity.cached_input_token_count = optional_int(step, "cachedInputTokenCount");
    entity.cache_write_input_token_count = optional_int(step, "cacheWriteInputTokenCount");
    entity.uncached_input_token_count = optional_int(step, "uncachedInputTokenCount");
    entity.output_token_count = optional_int(step, "outputTokenCount");
    entity.reasoning_token_count = optional_int(step, "reasoningTokenCount");
    entity.generation_duration_ms = optional_long(step, "generationDurationMs");
    const std::string status = optional_string(step, "status").value_or("SUCCESS");
    if (status == "TRANSCRIBING") entity.status = recovery::MessageStatus::transcribing;
    else if (status == "SENDING") entity.status = recovery::MessageStatus::sending;
    else if (status == "THINKING") entity.status = recovery::MessageStatus::thinking;
    else if (status == "TOOL_CALLING") entity.status = recovery::MessageStatus::tool_calling;
    else if (status == "SUCCESS") entity.status = recovery::MessageStatus::success;
    else if (status == "STOPPED") entity.status = recovery::MessageStatus::stopped;
    else if (status == "ERROR") entity.status = recovery::MessageStatus::error;
    else throw KotlinError("IllegalArgumentException", "Unknown MessageStatus " + status);
    const std::string participant = optional_string(step, "participant").value_or("MODEL");
    if (participant == "USER") entity.participant = recovery::Participant::user;
    else if (participant == "MODEL") entity.participant = recovery::Participant::model;
    else if (participant == "ERROR") entity.participant = recovery::Participant::error;
    else throw KotlinError("IllegalArgumentException", "Unknown Participant " + participant);
    entity.timestamp = optional_long(step, "timestamp").value_or(0);
    entity.thought_time_ms = optional_long(step, "thoughtTimeMs");
    entity.model_name = optional_string(step, "modelName");
    entity.tool_call_json = optional_string(step, "toolCallJson");
    entity.attachment_meta = optional_string(step, "attachmentMeta");
    entity.run_id = required_string(step, "runId");
    entity.run_sequence = optional_long(step, "runSequence").value_or(-1);
    entity.consumed_at_pass = optional_int(step, "consumedAtPass");
    entity.source_json = optional_string(step, "sourceJson");
    return entity;
}

MessageStreamCheckpoint checkpoint(const json::Value& step) {
    MessageStreamCheckpoint entity;
    entity.id = required_string(step, "id");
    entity.text = optional_string(step, "text").value_or("");
    entity.images = string_list(step, "images");
    entity.thoughts = optional_string(step, "thoughts");
    entity.thought_title = optional_string(step, "thoughtTitle");
    entity.token_count = optional_int(step, "tokenCount").value_or(0);
    entity.input_token_count = optional_int(step, "inputTokenCount");
    entity.cached_input_token_count = optional_int(step, "cachedInputTokenCount");
    entity.cache_write_input_token_count = optional_int(step, "cacheWriteInputTokenCount");
    entity.uncached_input_token_count = optional_int(step, "uncachedInputTokenCount");
    entity.output_token_count = optional_int(step, "outputTokenCount");
    entity.reasoning_token_count = optional_int(step, "reasoningTokenCount");
    entity.generation_duration_ms = optional_long(step, "generationDurationMs");
    const std::string status = optional_string(step, "status").value_or("SUCCESS");
    if (status == "TRANSCRIBING") entity.status = recovery::MessageStatus::transcribing;
    else if (status == "SENDING") entity.status = recovery::MessageStatus::sending;
    else if (status == "THINKING") entity.status = recovery::MessageStatus::thinking;
    else if (status == "TOOL_CALLING") entity.status = recovery::MessageStatus::tool_calling;
    else if (status == "SUCCESS") entity.status = recovery::MessageStatus::success;
    else if (status == "STOPPED") entity.status = recovery::MessageStatus::stopped;
    else if (status == "ERROR") entity.status = recovery::MessageStatus::error;
    else throw KotlinError("IllegalArgumentException", "Unknown MessageStatus " + status);
    entity.thought_time_ms = optional_long(step, "thoughtTimeMs");
    entity.tool_call_json = optional_string(step, "toolCallJson");
    return entity;
}

EmbeddingEntity embedding(const json::Value& step) {
    EmbeddingEntity entity;
    entity.id = optional_long(step, "id").value_or(0);
    entity.message_id = required_string(step, "messageId");
    entity.model_id = required_string(step, "modelId");
    entity.embedding = base64_decode(optional_string(step, "embeddingBase64").value_or(""));
    entity.chunk_text = optional_string(step, "chunkText").value_or("");
    entity.dimension = optional_int(step, "dimension").value_or(0);
    entity.source_fingerprint = optional_string(step, "sourceFingerprint");
    return entity;
}

automation::TaskEntity task(const json::Value& step) {
    automation::TaskEntity entity;
    entity.id = required_string(step, "id");
    entity.name = optional_string(step, "name").value_or("");
    entity.prompt = optional_string(step, "prompt").value_or("");
    entity.system_prompt = optional_string(step, "systemPrompt");
    entity.system_prompt_id = optional_string(step, "systemPromptId");
    entity.model_id = optional_string(step, "modelId");
    entity.cron_expr = optional_string(step, "cronExpr").value_or("");
    entity.run_at = optional_long(step, "runAt");
    entity.next_run_at = optional_long(step, "nextRunAt").value_or(0);
    entity.enabled = optional_bool(step, "enabled").value_or(true);
    entity.created_at = optional_long(step, "createdAt").value_or(0);
    entity.last_run_at = optional_long(step, "lastRunAt");
    return entity;
}

automation::LoopEntity loop(const json::Value& step) {
    automation::LoopEntity entity;
    entity.conversation_id = required_string(step, "conversationId");
    entity.interval_ms = optional_long(step, "intervalMs").value_or(0);
    entity.prompt = optional_string(step, "prompt");
    entity.next_fire_at = optional_long(step, "nextFireAt").value_or(0);
    entity.cycle_count = optional_int(step, "cycleCount").value_or(0);
    entity.max_cycles = optional_int(step, "maxCycles");
    entity.active = optional_bool(step, "active").value_or(true);
    entity.revision = optional_long(step, "revision").value_or(0);
    return entity;
}

new_chat::NewChatPersistEntity new_chat_persist(const json::Value& step) {
    new_chat::NewChatPersistEntity entity;
    entity.id = optional_int(step, "id").value_or(0);
    entity.model_id = optional_string(step, "modelId");
    entity.system_prompt_id = optional_string(step, "systemPromptId");
    entity.conversation_settings_json = optional_string(step, "conversationSettingsJson");
    entity.draft_text = optional_string(step, "draftText").value_or("");
    entity.draft_attachments = optional_string(step, "draftAttachments");
    return entity;
}

ConversationSettingsTransferEntity settings_transfer(const json::Value& step) {
    ConversationSettingsTransferEntity entity;
    entity.conversation_id = required_string(step, "conversationId");
    entity.settings_json = optional_string(step, "settingsJson");
    return entity;
}

ConversationSettingsImportTransferEntity import_transfer(const json::Value& step) {
    ConversationSettingsImportTransferEntity entity;
    entity.id = optional_int(step, "id").value_or(0);
    entity.transfer_id = required_string(step, "transferId");
    entity.settings_json = optional_string(step, "settingsJson").value_or("");
    entity.mode = required_string(step, "mode");
    return entity;
}

// ---- entity rendering ---------------------------------------------------------------------

json::Value chat_conversation_json(const ChatConversation& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"title", json_string(entity.title)},
        {"systemPromptId", optional_json_string(entity.system_prompt_id)},
        {"modelId", optional_json_string(entity.model_id)},
        {"taskId", optional_json_string(entity.task_id)},
        {"origin", json_string(entity.origin)},
        {"graduated", json_bool(entity.graduated)},
        {"hasUnreadGeneration", json_bool(entity.has_unread_generation)},
        {"isPinned", json_bool(entity.is_pinned)},
        {"selectedBranchesJson", optional_json_string(entity.selected_branches_json)},
    });
}

json::Value drawer_json(const DrawerConversationRow& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"title", json_string(entity.title)},
        {"hasUnreadGeneration", json_bool(entity.has_unread_generation)},
        {"isPinned", json_bool(entity.is_pinned)},
    });
}

json::Value conversation_json(const ChatEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"title", json_string(entity.title)},
        {"lastUpdated", json_long(entity.last_updated)},
        {"dataChangedAt", json_long(entity.data_changed_at)},
        {"selectedBranchesJson", optional_json_string(entity.selected_branches_json)},
        {"systemPromptId", optional_json_string(entity.system_prompt_id)},
        {"modelId", optional_json_string(entity.model_id)},
        {"taskId", optional_json_string(entity.task_id)},
        {"origin", json_string(entity.origin)},
        {"graduated", json_bool(entity.graduated)},
        {"draftText", json_string(entity.draft_text)},
        {"draftAttachments", optional_json_string(entity.draft_attachments)},
        {"selectedRunBranchesJson", optional_json_string(entity.selected_run_branches_json)},
        {"hasUnreadGeneration", json_bool(entity.has_unread_generation)},
        {"isPinned", json_bool(entity.is_pinned)},
    });
}

const char* run_status_name(runtime::RunStatus status) {
    switch (status) {
        case runtime::RunStatus::Active: return "ACTIVE";
        case runtime::RunStatus::Stopping: return "STOPPING";
        case runtime::RunStatus::Completed: return "COMPLETED";
        case runtime::RunStatus::Stopped: return "STOPPED";
        case runtime::RunStatus::Failed: return "FAILED";
    }
    return "ACTIVE";
}

const char* run_reason_name(runtime::RunEndReason reason) {
    switch (reason) {
        case runtime::RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case runtime::RunEndReason::UserStopped: return "USER_STOPPED";
        case runtime::RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case runtime::RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    return "MODEL_COMPLETED";
}

const char* message_status_name(recovery::MessageStatus status) {
    switch (status) {
        case recovery::MessageStatus::transcribing: return "TRANSCRIBING";
        case recovery::MessageStatus::sending: return "SENDING";
        case recovery::MessageStatus::thinking: return "THINKING";
        case recovery::MessageStatus::tool_calling: return "TOOL_CALLING";
        case recovery::MessageStatus::success: return "SUCCESS";
        case recovery::MessageStatus::stopped: return "STOPPED";
        case recovery::MessageStatus::error: return "ERROR";
    }
    return "SUCCESS";
}

const char* participant_name(recovery::Participant participant) {
    switch (participant) {
        case recovery::Participant::user: return "USER";
        case recovery::Participant::model: return "MODEL";
        case recovery::Participant::error: return "ERROR";
    }
    return "MODEL";
}

json::Value run_json(const RunEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"conversationId", json_string(entity.conversation_id)},
        {"parentRunId", optional_json_string(entity.parent_run_id)},
        {"status", json_string(run_status_name(entity.status))},
        {"activeSlot", optional_json_long(entity.active_slot)},
        {"startedAt", json_long(entity.started_at)},
        {"lastCheckpointAt", json_long(entity.last_checkpoint_at)},
        {"stopRequestedAt", optional_json_long(entity.stop_requested_at)},
        {"endedAt", optional_json_long(entity.ended_at)},
        {"endReason", entity.end_reason.has_value()
                           ? json_string(run_reason_name(*entity.end_reason))
                           : json_null()},
        {"currentPass", json_long(entity.current_pass)},
        {"legacyAmbiguous", json_bool(entity.legacy_ambiguous)},
    });
}

json::Value message_json(const MessageEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"conversationId", json_string(entity.conversation_id)},
        {"parentId", optional_json_string(entity.parent_id)},
        {"text", json_string(entity.text)},
        {"images", string_array(entity.images)},
        {"thoughts", optional_json_string(entity.thoughts)},
        {"thoughtTitle", optional_json_string(entity.thought_title)},
        {"tokenCount", json_long(entity.token_count)},
        {"inputTokenCount", optional_json_int(entity.input_token_count)},
        {"cachedInputTokenCount", optional_json_int(entity.cached_input_token_count)},
        {"cacheWriteInputTokenCount", optional_json_int(entity.cache_write_input_token_count)},
        {"uncachedInputTokenCount", optional_json_int(entity.uncached_input_token_count)},
        {"outputTokenCount", optional_json_int(entity.output_token_count)},
        {"reasoningTokenCount", optional_json_int(entity.reasoning_token_count)},
        {"generationDurationMs", optional_json_long(entity.generation_duration_ms)},
        {"status", json_string(message_status_name(entity.status))},
        {"participant", json_string(participant_name(entity.participant))},
        {"timestamp", json_long(entity.timestamp)},
        {"thoughtTimeMs", optional_json_long(entity.thought_time_ms)},
        {"modelName", optional_json_string(entity.model_name)},
        {"toolCallJson", optional_json_string(entity.tool_call_json)},
        {"attachmentMeta", optional_json_string(entity.attachment_meta)},
        {"runId", json_string(entity.run_id)},
        {"runSequence", json_long(entity.run_sequence)},
        {"consumedAtPass", optional_json_int(entity.consumed_at_pass)},
        {"sourceJson", optional_json_string(entity.source_json)},
    });
}

json::Value embedding_json(const EmbeddingEntity& entity) {
    return json_object({
        {"id", json_long(entity.id)},
        {"messageId", json_string(entity.message_id)},
        {"modelId", json_string(entity.model_id)},
        {"embeddingBase64", json_string(base64_encode(entity.embedding))},
        {"chunkText", json_string(entity.chunk_text)},
        {"dimension", json_long(entity.dimension)},
        {"sourceFingerprint", optional_json_string(entity.source_fingerprint)},
    });
}

json::Value settings_transfer_json(const ConversationSettingsTransferEntity& entity) {
    return json_object({
        {"conversationId", json_string(entity.conversation_id)},
        {"settingsJson", optional_json_string(entity.settings_json)},
    });
}

json::Value import_transfer_json(const ConversationSettingsImportTransferEntity& entity) {
    return json_object({
        {"id", json_long(entity.id)},
        {"transferId", json_string(entity.transfer_id)},
        {"settingsJson", json_string(entity.settings_json)},
        {"mode", json_string(entity.mode)},
    });
}

json::Value execution_json(const ExecutionMessageSummaryRow& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"conversationId", json_string(entity.conversation_id)},
        {"participant", json_string(participant_name(entity.participant))},
        {"status", json_string(message_status_name(entity.status))},
        {"timestamp", json_long(entity.timestamp)},
        {"preview", json_string(entity.preview)},
    });
}

json::Value task_json(const automation::TaskEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"name", json_string(entity.name)},
        {"prompt", json_string(entity.prompt)},
        {"systemPrompt", optional_json_string(entity.system_prompt)},
        {"systemPromptId", optional_json_string(entity.system_prompt_id)},
        {"modelId", optional_json_string(entity.model_id)},
        {"cronExpr", json_string(entity.cron_expr)},
        {"runAt", optional_json_long(entity.run_at)},
        {"nextRunAt", json_long(entity.next_run_at)},
        {"enabled", json_bool(entity.enabled)},
        {"createdAt", json_long(entity.created_at)},
        {"lastRunAt", optional_json_long(entity.last_run_at)},
    });
}

json::Value loop_json(const automation::LoopEntity& entity) {
    return json_object({
        {"conversationId", json_string(entity.conversation_id)},
        {"intervalMs", json_long(entity.interval_ms)},
        {"prompt", optional_json_string(entity.prompt)},
        {"nextFireAt", json_long(entity.next_fire_at)},
        {"cycleCount", json_long(entity.cycle_count)},
        {"maxCycles", optional_json_int(entity.max_cycles)},
        {"active", json_bool(entity.active)},
        {"revision", json_long(entity.revision)},
    });
}

json::Value new_chat_json(const new_chat::NewChatPersistEntity& entity) {
    return json_object({
        {"id", json_long(entity.id)},
        {"modelId", optional_json_string(entity.model_id)},
        {"systemPromptId", optional_json_string(entity.system_prompt_id)},
        {"conversationSettingsJson",
         optional_json_string(entity.conversation_settings_json)},
        {"draftText", json_string(entity.draft_text)},
        {"draftAttachments", optional_json_string(entity.draft_attachments)},
    });
}

json::Value selection_map_json(const SelectionMap& selections) {
    std::vector<std::pair<std::string, json::Value>> members;
    for (const auto& [key, value] : selections) {
        members.emplace_back(key.value_or("null"), json_string(value));
    }
    return json_object(std::move(members));
}

json::Value run_graph_commit_json(const RunGraphCommit& commit) {
    std::vector<json::Value> messages;
    for (const MessageEntity& message : commit.messages) messages.push_back(message_json(message));
    return json_object({
        {"messages", json_array(std::move(messages))},
        {"messageSelections", selection_map_json(commit.message_selections)},
        {"runSelections", selection_map_json(commit.run_selections)},
    });
}

json::Value tool_round_commit_json(const ToolRoundCommit& commit) {
    std::vector<json::Value> messages;
    for (const MessageEntity& message : commit.messages) messages.push_back(message_json(message));
    return json_object({
        {"messages", json_array(std::move(messages))},
        {"inserted", json_bool(commit.inserted)},
    });
}

// ---- table dump ---------------------------------------------------------------------------

json::Value table_json(SqliteExecutor& sql, const std::string& query,
                       const std::function<json::Value(const SqlRow&)>& render) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(query, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value dump_tables(SqliteExecutor& sql, bool mask_clock) {
    return json_object({
        {"conversations", table_json(sql,
             "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, systemPromptId, "
             "modelId, taskId, origin, graduated, draftText, draftAttachments, "
             "selectedRunBranchesJson, hasUnreadGeneration, isPinned FROM conversations ORDER BY id",
             [&](const SqlRow& row) {
                 return json_object({
                     {"id", json_string(row[0].as_text())},
                     {"title", json_string(row[1].as_text())},
                     {"lastUpdated", json_long(row[2].as_integer())},
                     {"dataChangedAt", json_long(mask_clock ? 0 : row[3].as_integer())},
                     {"selectedBranchesJson", row[4].text_or_null().has_value()
                                                  ? json_string(*row[4].text_or_null())
                                                  : json_null()},
                     {"systemPromptId", row[5].text_or_null().has_value()
                                             ? json_string(*row[5].text_or_null())
                                             : json_null()},
                     {"modelId", row[6].text_or_null().has_value()
                                     ? json_string(*row[6].text_or_null())
                                     : json_null()},
                     {"taskId", row[7].text_or_null().has_value()
                                    ? json_string(*row[7].text_or_null())
                                    : json_null()},
                     {"origin", json_string(row[8].as_text())},
                     {"graduated", json_bool(row[9].as_integer() != 0)},
                     {"draftText", json_string(row[10].as_text())},
                     {"draftAttachments", row[11].text_or_null().has_value()
                                              ? json_string(*row[11].text_or_null())
                                              : json_null()},
                     {"selectedRunBranchesJson", row[12].text_or_null().has_value()
                                                      ? json_string(*row[12].text_or_null())
                                                      : json_null()},
                     {"hasUnreadGeneration", json_bool(row[13].as_integer() != 0)},
                     {"isPinned", json_bool(row[14].as_integer() != 0)},
                 });
             })},
        {"runs", table_json(sql,
             "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, "
             "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous "
             "FROM runs ORDER BY id",
             [](const SqlRow& row) {
                 return json_object({
                     {"id", json_string(row[0].as_text())},
                     {"conversationId", json_string(row[1].as_text())},
                     {"parentRunId", row[2].text_or_null().has_value()
                                          ? json_string(*row[2].text_or_null())
                                          : json_null()},
                     {"status", json_string(row[3].as_text())},
                     {"activeSlot", row[4].integer_or_null().has_value()
                                        ? json_long(*row[4].integer_or_null())
                                        : json_null()},
                     {"startedAt", json_long(row[5].as_integer())},
                     {"lastCheckpointAt", json_long(row[6].as_integer())},
                     {"stopRequestedAt", row[7].integer_or_null().has_value()
                                             ? json_long(*row[7].integer_or_null())
                                             : json_null()},
                     {"endedAt", row[8].integer_or_null().has_value()
                                     ? json_long(*row[8].integer_or_null())
                                     : json_null()},
                     {"endReason", row[9].text_or_null().has_value()
                                       ? json_string(*row[9].text_or_null())
                                       : json_null()},
                     {"currentPass", json_long(row[10].as_integer())},
                     {"legacyAmbiguous", json_bool(row[11].as_integer() != 0)},
                 });
             })},
        {"messages", table_json(sql,
             "SELECT id, conversationId, parentId, text, images, thoughts, thoughtTitle, tokenCount, "
             "inputTokenCount, cachedInputTokenCount, cacheWriteInputTokenCount, "
             "uncachedInputTokenCount, outputTokenCount, reasoningTokenCount, generationDurationMs, "
             "status, participant, timestamp, thoughtTimeMs, modelName, toolCallJson, "
             "attachmentMeta, runId, runSequence, consumedAtPass, sourceJson FROM messages ORDER BY id",
             [](const SqlRow& row) {
                 auto text_or_null = [](const SqlValue& cell) {
                     return cell.text_or_null().has_value() ? json_string(*cell.text_or_null())
                                                            : json_null();
                 };
                 auto int_or_null = [](const SqlValue& cell) {
                     return cell.integer_or_null().has_value()
                                ? json_long(*cell.integer_or_null())
                                : json_null();
                 };
                 return json_object({
                     {"id", json_string(row[0].as_text())},
                     {"conversationId", json_string(row[1].as_text())},
                     {"parentId", text_or_null(row[2])},
                     {"text", json_string(row[3].as_text())},
                     {"images", json_string(row[4].as_text())},
                     {"thoughts", text_or_null(row[5])},
                     {"thoughtTitle", text_or_null(row[6])},
                     {"tokenCount", json_long(row[7].as_integer())},
                     {"inputTokenCount", int_or_null(row[8])},
                     {"cachedInputTokenCount", int_or_null(row[9])},
                     {"cacheWriteInputTokenCount", int_or_null(row[10])},
                     {"uncachedInputTokenCount", int_or_null(row[11])},
                     {"outputTokenCount", int_or_null(row[12])},
                     {"reasoningTokenCount", int_or_null(row[13])},
                     {"generationDurationMs", int_or_null(row[14])},
                     {"status", json_string(row[15].as_text())},
                     {"participant", json_string(row[16].as_text())},
                     {"timestamp", json_long(row[17].as_integer())},
                     {"thoughtTimeMs", int_or_null(row[18])},
                     {"modelName", text_or_null(row[19])},
                     {"toolCallJson", text_or_null(row[20])},
                     {"attachmentMeta", text_or_null(row[21])},
                     {"runId", json_string(row[22].as_text())},
                     {"runSequence", json_long(row[23].as_integer())},
                     {"consumedAtPass", int_or_null(row[24])},
                     {"sourceJson", text_or_null(row[25])},
                 });
             })},
        {"embeddings", table_json(sql,
             "SELECT id, messageId, modelId, embedding, chunkText, dimension, sourceFingerprint "
             "FROM embeddings ORDER BY id",
             [](const SqlRow& row) {
                 return json_object({
                     {"id", json_long(row[0].as_integer())},
                     {"messageId", json_string(row[1].as_text())},
                     {"modelId", json_string(row[2].as_text())},
                     {"embeddingBase64", json_string(base64_encode(row[3].as_blob()))},
                     {"chunkText", json_string(row[4].as_text())},
                     {"dimension", json_long(row[5].as_integer())},
                     {"sourceFingerprint", row[6].text_or_null().has_value()
                                               ? json_string(*row[6].text_or_null())
                                               : json_null()},
                 });
             })},
        {"tasks", table_json(sql,
             "SELECT id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, runAt, "
             "nextRunAt, enabled, createdAt, lastRunAt FROM tasks ORDER BY id",
             [](const SqlRow& row) {
                 auto text_or_null = [](const SqlValue& cell) {
                     return cell.text_or_null().has_value() ? json_string(*cell.text_or_null())
                                                            : json_null();
                 };
                 auto long_or_null = [](const SqlValue& cell) {
                     return cell.integer_or_null().has_value()
                                ? json_long(*cell.integer_or_null())
                                : json_null();
                 };
                 return json_object({
                     {"id", json_string(row[0].as_text())},
                     {"name", json_string(row[1].as_text())},
                     {"prompt", json_string(row[2].as_text())},
                     {"systemPrompt", text_or_null(row[3])},
                     {"systemPromptId", text_or_null(row[4])},
                     {"modelId", text_or_null(row[5])},
                     {"cronExpr", json_string(row[6].as_text())},
                     {"runAt", long_or_null(row[7])},
                     {"nextRunAt", json_long(row[8].as_integer())},
                     {"enabled", json_bool(row[9].as_integer() != 0)},
                     {"createdAt", json_long(row[10].as_integer())},
                     {"lastRunAt", long_or_null(row[11])},
                 });
             })},
        {"loops", table_json(sql,
             "SELECT conversationId, intervalMs, prompt, nextFireAt, cycleCount, maxCycles, active, "
             "revision FROM loops ORDER BY conversationId",
             [](const SqlRow& row) {
                 return json_object({
                     {"conversationId", json_string(row[0].as_text())},
                     {"intervalMs", json_long(row[1].as_integer())},
                     {"prompt", row[2].text_or_null().has_value()
                                    ? json_string(*row[2].text_or_null())
                                    : json_null()},
                     {"nextFireAt", json_long(row[3].as_integer())},
                     {"cycleCount", json_long(row[4].as_integer())},
                     {"maxCycles", row[5].integer_or_null().has_value()
                                       ? json_long(*row[5].integer_or_null())
                                       : json_null()},
                     {"active", json_bool(row[6].as_integer() != 0)},
                     {"revision", json_long(row[7].as_integer())},
                 });
             })},
        {"new_chat_persist", table_json(sql,
             "SELECT id, modelId, systemPromptId, conversationSettingsJson, draftText, "
             "draftAttachments FROM new_chat_persist ORDER BY id",
             [](const SqlRow& row) {
                 auto text_or_null = [](const SqlValue& cell) {
                     return cell.text_or_null().has_value() ? json_string(*cell.text_or_null())
                                                            : json_null();
                 };
                 return json_object({
                     {"id", json_long(row[0].as_integer())},
                     {"modelId", text_or_null(row[1])},
                     {"systemPromptId", text_or_null(row[2])},
                     {"conversationSettingsJson", text_or_null(row[3])},
                     {"draftText", json_string(row[4].as_text())},
                     {"draftAttachments", text_or_null(row[5])},
                 });
             })},
        {"conversation_settings_transfer", table_json(sql,
             "SELECT conversationId, settingsJson FROM conversation_settings_transfer "
             "ORDER BY conversationId",
             [](const SqlRow& row) {
                 return json_object({
                     {"conversationId", json_string(row[0].as_text())},
                     {"settingsJson", row[1].text_or_null().has_value()
                                          ? json_string(*row[1].text_or_null())
                                          : json_null()},
                 });
             })},
        {"conversation_settings_import_transfer", table_json(sql,
             "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer "
             "ORDER BY id",
             [](const SqlRow& row) {
                 return json_object({
                     {"id", json_long(row[0].as_integer())},
                     {"transferId", json_string(row[1].as_text())},
                     {"settingsJson", json_string(row[2].as_text())},
                     {"mode", json_string(row[3].as_text())},
                 });
             })},
    });
}

// ---- seeding ------------------------------------------------------------------------------

void seed(ChatDao& dao, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* rows = field(*seed_object, "conversations"); rows != nullptr) {
        for (const json::Value& row : rows->elements) dao.upsert_conversation(conversation(row));
    }
    if (const json::Value* rows = field(*seed_object, "runs"); rows != nullptr) {
        for (const json::Value& row : rows->elements) dao.upsert_run(run(row));
    }
    if (const json::Value* rows = field(*seed_object, "messages"); rows != nullptr) {
        for (const json::Value& row : rows->elements) dao.upsert_message(message(row));
    }
    if (const json::Value* rows = field(*seed_object, "tasks"); rows != nullptr) {
        for (const json::Value& row : rows->elements) dao.upsert_task(task(row));
    }
    if (const json::Value* rows = field(*seed_object, "loops"); rows != nullptr) {
        for (const json::Value& row : rows->elements) dao.upsert_loop_row(loop(row));
    }
    if (const json::Value* rows = field(*seed_object, "settingsTransfers"); rows != nullptr) {
        for (const json::Value& row : rows->elements) {
            dao.upsert_conversation_settings_transfer(settings_transfer(row));
        }
    }
    if (const json::Value* row = field(*seed_object, "importTransfer"); row != nullptr) {
        dao.upsert_conversation_settings_import_transfer(import_transfer(*row));
    }
    if (const json::Value* row = field(*seed_object, "newChatPersist"); row != nullptr) {
        dao.upsert_new_chat_persist(new_chat_persist(*row));
    }
    if (const json::Value* rows = field(*seed_object, "embeddings"); rows != nullptr) {
        std::vector<EmbeddingEntity> embeddings;
        for (const json::Value& row : rows->elements) embeddings.push_back(embedding(row));
        if (!embeddings.empty()) dao.insert_embeddings(embeddings);
    }
}

// ---- operation execution ------------------------------------------------------------------

json::Value execute_operation(const std::string& op, const json::Value& step, ChatDao& dao) {
    if (op == "getAllConversations") {
        std::vector<json::Value> rows;
        for (const ChatConversation& row : dao.get_all_conversations()) {
            rows.push_back(chat_conversation_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "observeDrawerConversations") {
        std::vector<json::Value> rows;
        for (const DrawerConversationRow& row : dao.observe_drawer_conversations(required_int(step, "limit"))) {
            rows.push_back(drawer_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getDrawerConversations") {
        std::vector<json::Value> rows;
        for (const DrawerConversationRow& row : dao.get_drawer_conversations(string_list(step, "ids"))) {
            rows.push_back(drawer_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getExecutionsForTask") {
        std::vector<json::Value> rows;
        for (const ChatEntity& row : dao.get_executions_for_task(required_string(step, "taskId"))) {
            rows.push_back(conversation_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "observeConversation") {
        const std::optional<ChatEntity> row = dao.observe_conversation(required_string(step, "conversationId"));
        return row.has_value() ? conversation_json(*row) : json_null();
    }
    if (op == "getConversationSettingsTransfer") {
        const std::optional<ConversationSettingsTransferEntity> row =
            dao.get_conversation_settings_transfer(required_string(step, "conversationId"));
        return row.has_value() ? settings_transfer_json(*row) : json_null();
    }
    if (op == "getPendingConversationSettingsTransfers") {
        std::vector<json::Value> rows;
        for (const auto& row : dao.get_pending_conversation_settings_transfers()) {
            rows.push_back(settings_transfer_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "upsertConversationSettingsTransfer") {
        dao.upsert_conversation_settings_transfer(settings_transfer(step));
        return json_null();
    }
    if (op == "deleteConversationSettingsTransfer") {
        return json_long(dao.delete_conversation_settings_transfer(required_string(step, "conversationId")));
    }
    if (op == "getConversationSettingsImportTransfer") {
        const auto row = dao.get_conversation_settings_import_transfer();
        return row.has_value() ? import_transfer_json(*row) : json_null();
    }
    if (op == "upsertConversationSettingsImportTransfer") {
        dao.upsert_conversation_settings_import_transfer(import_transfer(step));
        return json_null();
    }
    if (op == "deleteConversationSettingsImportTransfer") {
        return json_long(dao.delete_conversation_settings_import_transfer(required_string(step, "transferId")));
    }
    if (op == "observeMessage") {
        const auto row = dao.observe_message(required_string(step, "messageId"));
        return row.has_value() ? message_json(*row) : json_null();
    }
    if (op == "stopStuckMessagesForConversation") {
        return json_long(dao.stop_stuck_messages_for_conversation(required_string(step, "conversationId")));
    }
    if (op == "upsertConversation") {
        dao.upsert_conversation(conversation(step));
        return json_null();
    }
    if (op == "updateConversationModel") {
        return json_long(dao.update_conversation_model(required_string(step, "conversationId"),
                                                       optional_string(step, "modelId"),
                                                       required_long(step, "at")));
    }
    if (op == "updateConversationSystemPrompt") {
        return json_long(dao.update_conversation_system_prompt(required_string(step, "conversationId"),
                                                               optional_string(step, "promptId"),
                                                               required_long(step, "at")));
    }
    if (op == "touchConversationData") {
        return json_long(dao.touch_conversation_data(required_string(step, "conversationId"),
                                                      required_long(step, "at")));
    }
    if (op == "upsertLoop") {
        dao.upsert_loop(loop(step), 0);
        return json_null();
    }
    if (op == "deactivateLoopIfUnchanged") {
        return json_long(dao.deactivate_loop_if_unchanged(
            required_string(step, "conversationId"), required_long(step, "expectedRevision"),
            required_int(step, "expectedCycleCount"), required_long(step, "expectedIntervalMs"),
            required_long(step, "expectedNextFireAt"), required_int(step, "normalizedMaxCycles"), 0));
    }
    if (op == "deleteLoop") {
        dao.delete_loop(required_string(step, "conversationId"), 0);
        return json_null();
    }
    if (op == "updateConversationTitle") {
        return json_long(dao.update_conversation_title(required_string(step, "conversationId"),
                                                       required_string(step, "title"),
                                                       required_long(step, "at")));
    }
    if (op == "setConversationPinned") {
        return json_long(dao.set_conversation_pinned(required_string(step, "conversationId"),
                                                     required_bool(step, "pinned"),
                                                     required_long(step, "at")));
    }
    if (op == "setConversationUnreadGeneration") {
        return json_long(dao.set_conversation_unread_generation(required_string(step, "conversationId"),
                                                                required_bool(step, "unread")));
    }
    if (op == "replaceConversationModelReferences") {
        return json_long(dao.replace_conversation_model_references(
            required_string(step, "oldModelId"), optional_string(step, "newModelId"),
            required_long(step, "at")));
    }
    if (op == "replaceNewChatModelReference") {
        return json_long(dao.replace_new_chat_model_reference(required_string(step, "oldModelId"),
                                                              optional_string(step, "newModelId")));
    }
    if (op == "updateConversationTitleIfUnchanged") {
        return json_long(dao.update_conversation_title_if_unchanged(
            required_string(step, "conversationId"), required_string(step, "expectedTitle"),
            required_string(step, "newTitle"), required_long(step, "at")));
    }
    if (op == "upsertMessage") {
        dao.upsert_message(message(step));
        return json_null();
    }
    if (op == "upsertRun") {
        dao.upsert_run(run(step));
        return json_null();
    }
    if (op == "getRunsForConversation") {
        std::vector<json::Value> rows;
        for (const RunEntity& row : dao.get_runs_for_conversation(required_string(step, "conversationId"))) {
            rows.push_back(run_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getRunsForConversationSnapshot") {
        std::vector<json::Value> rows;
        for (const RunEntity& row : dao.get_runs_for_conversation_snapshot(required_string(step, "conversationId"))) {
            rows.push_back(run_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "updateConversationForRunAdmission") {
        return json_long(dao.update_conversation_for_run_admission(
            required_string(step, "conversationId"), required_string(step, "selectedBranchesJson"),
            required_string(step, "selectedRunBranchesJson"), optional_string(step, "modelId"),
            required_long(step, "at"), required_bool(step, "touchConversationOnAdmission")));
    }
    if (op == "deleteMessageSubtree") {
        return json_bool(dao.delete_message_subtree(
            required_string(step, "conversationId"), required_string(step, "rootMessageId"),
            string_list(step, "staleMessageIds"), string_list(step, "rootRunIdsToDelete"),
            required_string(step, "selectedBranchesJson"),
            required_string(step, "selectedRunBranchesJson"), required_long(step, "at")));
    }
    if (op == "createRunWithMessages") {
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        return run_graph_commit_json(dao.create_run_with_messages(
            run(required_object(step, "run")), messages, selection_map(step, "messageSelectionUpdates"),
            optional_string(step, "conversationModelId"), required_long(step, "at"),
            required_bool(step, "touchConversationOnAdmission")));
    }
    if (op == "createConversationRunWithMessages") {
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        const json::Value* expected = optional_object(step, "expectedNewChatPersist");
        std::optional<new_chat::NewChatPersistEntity> expected_persist;
        if (expected != nullptr) expected_persist = new_chat_persist(*expected);
        return run_graph_commit_json(dao.create_conversation_run_with_messages(
            conversation(required_object(step, "conversation")), run(required_object(step, "run")),
            messages, selection_map(step, "messageSelectionUpdates"),
            required_string(step, "conversationModelId"),
            optional_string(step, "conversationSettingsJson"), expected_persist,
            required_long(step, "at")));
    }
    if (op == "importRunGraph") {
        std::vector<RunEntity> runs;
        for (const json::Value& row : objects(step, "runs")) runs.push_back(run(row));
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        dao.import_run_graph(runs, messages);
        return json_null();
    }
    if (op == "replaceImportedConversationGraph") {
        std::vector<ChatEntity> conversations;
        for (const json::Value& row : objects(step, "conversations")) conversations.push_back(conversation(row));
        std::vector<RunEntity> runs;
        for (const json::Value& row : objects(step, "runs")) runs.push_back(run(row));
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        dao.replace_imported_conversation_graph(conversations, runs, messages);
        return json_null();
    }
    if (op == "createForkGraph") {
        std::vector<RunEntity> runs;
        for (const json::Value& row : objects(step, "runs")) runs.push_back(run(row));
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        dao.create_fork_graph(conversation(required_object(step, "conversation")), runs, messages,
                              string_map(step, "sourceToForkMessageIds"));
        return json_null();
    }
    if (op == "appendToolRoundToRun") {
        std::vector<MessageEntity> messages;
        for (const json::Value& row : objects(step, "messages")) messages.push_back(message(row));
        return tool_round_commit_json(
            dao.append_tool_round_to_run(messages, required_int(step, "expectedPass"), 0));
    }
    if (op == "markRunStopping") {
        return json_long(dao.mark_run_stopping(required_string(step, "runId"), required_long(step, "at")));
    }
    if (op == "terminalizeLiveRun") {
        const std::string status = required_string(step, "status");
        const std::string reason = required_string(step, "reason");
        runtime::RunStatus run_status = runtime::RunStatus::Active;
        if (status == "ACTIVE") run_status = runtime::RunStatus::Active;
        else if (status == "STOPPING") run_status = runtime::RunStatus::Stopping;
        else if (status == "COMPLETED") run_status = runtime::RunStatus::Completed;
        else if (status == "STOPPED") run_status = runtime::RunStatus::Stopped;
        else if (status == "FAILED") run_status = runtime::RunStatus::Failed;
        else throw KotlinError("IllegalArgumentException", "Unknown RunStatus " + status);
        runtime::RunEndReason end_reason = runtime::RunEndReason::ModelCompleted;
        if (reason == "MODEL_COMPLETED") end_reason = runtime::RunEndReason::ModelCompleted;
        else if (reason == "USER_STOPPED") end_reason = runtime::RunEndReason::UserStopped;
        else if (reason == "PROCESS_RECOVERED") end_reason = runtime::RunEndReason::ProcessRecovered;
        else if (reason == "PROVIDER_ERROR") end_reason = runtime::RunEndReason::ProviderError;
        else throw KotlinError("IllegalArgumentException", "Unknown RunEndReason " + reason);
        return json_long(dao.terminalize_live_run(required_string(step, "runId"), run_status,
                                                  end_reason, required_long(step, "at")));
    }
    if (op == "stopInFlightModelMessages") {
        return json_long(dao.stop_in_flight_model_messages(required_string(step, "runId")));
    }
    if (op == "finishGeneration") {
        const std::string status = required_string(step, "status");
        const std::string reason = required_string(step, "reason");
        runtime::RunStatus run_status = status == "COMPLETED" ? runtime::RunStatus::Completed
                                         : status == "STOPPED" ? runtime::RunStatus::Stopped
                                         : status == "FAILED" ? runtime::RunStatus::Failed
                                         : runtime::RunStatus::Active;
        runtime::RunEndReason end_reason = reason == "MODEL_COMPLETED" ? runtime::RunEndReason::ModelCompleted
                                            : reason == "USER_STOPPED" ? runtime::RunEndReason::UserStopped
                                            : reason == "PROCESS_RECOVERED" ? runtime::RunEndReason::ProcessRecovered
                                            : runtime::RunEndReason::ProviderError;
        return json_bool(dao.finish_generation(
            checkpoint(required_object(step, "checkpoint")), required_string(step, "conversationId"),
            required_string(step, "runId"), run_status, end_reason, required_long(step, "at"),
            required_bool(step, "markConversationUnread")));
    }
    if (op == "finishStoppedGeneration") {
        std::vector<MessageStreamCheckpoint> checkpoints;
        for (const json::Value& row : objects(step, "checkpoints")) checkpoints.push_back(checkpoint(row));
        return json_bool(dao.finish_stopped_generation(
            checkpoints, required_string(step, "conversationId"), optional_string(step, "runId"),
            required_long(step, "at")));
    }
    if (op == "recoverConversationRuntime") {
        return json_long(dao.recover_conversation_runtime(required_string(step, "conversationId"),
                                                          required_long(step, "at")));
    }
    if (op == "updateConversationMessageCheckpoint") {
        return json_bool(dao.update_conversation_message_checkpoint(
            required_string(step, "conversationId"), checkpoint(required_object(step, "checkpoint")),
            required_long(step, "at")));
    }
    if (op == "updateMessageCheckpoint") {
        return json_long(dao.update_message_checkpoint(checkpoint(required_object(step, "checkpoint"))));
    }
    if (op == "deleteConversation") {
        dao.delete_conversation(required_string(step, "conversationId"));
        return json_null();
    }
    if (op == "deleteMessagesByConversation") {
        dao.delete_messages_by_conversation(required_string(step, "conversationId"));
        return json_null();
    }
    if (op == "updateRunParent") {
        return json_long(dao.update_run_parent(required_string(step, "runId"),
                                                optional_string(step, "newParentRunId")));
    }
    if (op == "deleteEmbeddingsByConversation") {
        dao.delete_embeddings_by_conversation(required_string(step, "conversationId"));
        return json_null();
    }
    if (op == "deleteOrphanedEmbeddings") {
        dao.delete_orphaned_embeddings();
        return json_null();
    }
    if (op == "getLastMessageForConversation") {
        const auto row = dao.get_last_message_for_conversation(required_string(step, "conversationId"));
        return row.has_value() ? message_json(*row) : json_null();
    }
    if (op == "observeExecutionMessagesForTask") {
        std::vector<json::Value> rows;
        for (const ExecutionMessageSummaryRow& row : dao.observe_execution_messages_for_task(required_string(step, "taskId"))) {
            rows.push_back(execution_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "insertEmbeddings") {
        std::vector<EmbeddingEntity> embeddings;
        for (const json::Value& row : objects(step, "embeddings")) embeddings.push_back(embedding(row));
        std::vector<json::Value> ids;
        for (const std::int64_t id : dao.insert_embeddings(embeddings)) ids.push_back(json_long(id));
        return json_array(std::move(ids));
    }
    if (op == "getEmbeddingsByMessageIds") {
        std::vector<json::Value> rows;
        for (const EmbeddingEntity& row : dao.get_embeddings_by_message_ids(string_list(step, "messageIds"))) {
            rows.push_back(embedding_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getEmbedding") {
        const auto row = dao.get_embedding(required_string(step, "messageId"));
        return row.has_value() ? embedding_json(*row) : json_null();
    }
    if (op == "getAllEmbeddings") {
        std::vector<json::Value> rows;
        for (const EmbeddingEntity& row : dao.get_all_embeddings()) rows.push_back(embedding_json(row));
        return json_array(std::move(rows));
    }
    if (op == "getMessagesByIds") {
        std::vector<json::Value> rows;
        for (const MessageEntity& row : dao.get_messages_by_ids(string_list(step, "ids"))) {
            rows.push_back(message_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "updateDraft") {
        dao.update_draft(required_string(step, "id"), required_string(step, "text"),
                         optional_string(step, "attachments"), required_long(step, "at"));
        return json_null();
    }
    if (op == "getAllConversationsList") {
        std::vector<json::Value> rows;
        for (const ChatEntity& row : dao.get_all_conversations_list()) rows.push_back(conversation_json(row));
        return json_array(std::move(rows));
    }
    if (op == "compareAndSetRunBranchSelections") {
        return json_long(dao.compare_and_set_run_branch_selections(
            required_string(step, "conversationId"), required_string(step, "expected"),
            required_string(step, "replacement")));
    }
    if (op == "getAllConversationIds") {
        std::vector<json::Value> rows;
        for (const std::string& id : dao.get_all_conversation_ids()) rows.push_back(json_string(id));
        return json_array(std::move(rows));
    }
    if (op == "getAllTaskIds") {
        std::vector<json::Value> rows;
        for (const std::string& id : dao.get_all_task_ids()) rows.push_back(json_string(id));
        return json_array(std::move(rows));
    }
    if (op == "getMessagesPage") {
        std::vector<json::Value> rows;
        for (const MessageEntity& row : dao.get_messages_page(optional_string(step, "afterId"),
                                                               required_int(step, "limit"))) {
            rows.push_back(message_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "getConversationMessagesPage") {
        std::vector<json::Value> rows;
        for (const MessageEntity& row : dao.get_conversation_messages_page(
                 required_string(step, "conversationId"), optional_string(step, "afterId"),
                 required_int(step, "limit"))) {
            rows.push_back(message_json(row));
        }
        return json_array(std::move(rows));
    }
    if (op == "deleteAllConversations") {
        dao.delete_all_conversations();
        return json_null();
    }
    if (op == "findExistingMessageIds") {
        std::vector<json::Value> rows;
        for (const std::string& id : dao.find_existing_message_ids(string_list(step, "ids"))) {
            rows.push_back(json_string(id));
        }
        return json_array(std::move(rows));
    }
    if (op == "replaceConfiguredModelReferences") {
        dao.replace_configured_model_references(required_string(step, "oldModelId"),
                                                 optional_string(step, "newModelId"), 0);
        return json_null();
    }
    if (op == "renameConversationProviderModelReferences") {
        return json_long(dao.rename_conversation_provider_model_references(
            required_string(step, "oldProvider"), required_string(step, "newProvider"),
            required_long(step, "at")));
    }
    if (op == "touchConversationsWithMessageProvider") {
        return json_long(dao.touch_conversations_with_message_provider(
            required_string(step, "oldProvider"), required_long(step, "at")));
    }
    if (op == "renameNewChatProviderModelReference") {
        return json_long(dao.rename_new_chat_provider_model_reference(
            required_string(step, "oldProvider"), required_string(step, "newProvider")));
    }
    if (op == "renameMessageProviderModelReferences") {
        return json_long(dao.rename_message_provider_model_references(
            required_string(step, "oldProvider"), required_string(step, "newProvider")));
    }
    if (op == "renameConfiguredProviderModelReferences") {
        dao.rename_configured_provider_model_references(required_string(step, "oldProvider"),
                                                         required_string(step, "newProvider"), 0);
        return json_null();
    }
    if (op == "getAllTasksList") {
        std::vector<json::Value> rows;
        for (const automation::TaskEntity& row : dao.get_all_tasks_list()) rows.push_back(task_json(row));
        return json_array(std::move(rows));
    }
    if (op == "getNewChatPersist") {
        const auto row = dao.get_new_chat_persist();
        return row.has_value() ? new_chat_json(*row) : json_null();
    }
    throw KotlinError("IllegalStateException", "unknown operation " + op);
}

json::Value error_json(const std::exception& error) {
    std::string class_name = "Exception";
    if (const auto* kotlin = dynamic_cast<const KotlinError*>(&error)) {
        class_name = kotlin->class_name();
    } else if (dynamic_cast<const SqlConstraintError*>(&error) != nullptr) {
        class_name = "SqlConstraintError";
    }
    return json_object({{"error", json_string(class_name)},
                        {"message", json_string(error.what())}});
}

std::string excerpt(const json::Value& value) {
    std::string text = json::to_kotlin_string(value);
    if (text.size() > 1600) text = text.substr(0, 1600) + "...";
    return text;
}

void run_scenario(const std::filesystem::path& scenario_path,
                  const std::filesystem::path& trace_path,
                  const std::filesystem::path& schema_path) {
    const json::ParseResult scenario_parsed = json::parse(read_file(scenario_path));
    const json::ParseResult trace_parsed = json::parse(read_file(trace_path));
    if (!scenario_parsed.value.has_value() || !trace_parsed.value.has_value()) {
        scenario_fail("cannot parse " + scenario_path.filename().string() + " or its trace");
    }
    const json::Value& scenario = *scenario_parsed.value;
    const json::Value& trace = *trace_parsed.value;
    const bool mask_clock = optional_bool(scenario, "maskClock").value_or(false);

    SqliteExecutor sql(schema_path);
    ChatDao dao(sql);
    seed(dao, scenario);

    const json::Value* scenario_steps_ptr = field(scenario, "steps");
    const json::Value* trace_steps_ptr = field(trace, "steps");
    if (scenario_steps_ptr == nullptr || trace_steps_ptr == nullptr ||
        scenario_steps_ptr->kind != json::Kind::Array ||
        trace_steps_ptr->kind != json::Kind::Array) {
        scenario_fail("missing steps in " + scenario_path.filename().string());
    }
    const json::Value& scenario_steps = *scenario_steps_ptr;
    const json::Value& trace_steps = *trace_steps_ptr;
    if (scenario_steps.elements.size() != trace_steps.elements.size()) {
        scenario_fail("step count mismatch in " + scenario_path.filename().string());
    }
    for (std::size_t index = 0; index < scenario_steps.elements.size(); ++index) {
        const json::Value& step = scenario_steps.elements[index];
        const json::Value& expected_step = trace_steps.elements[index];
        const std::string op = required_string(step, "op");
        const std::string expected_op = required_string(expected_step, "op");
        if (op != expected_op) {
            scenario_fail("trace op mismatch at step " + std::to_string(index));
        }
        json::Value actual;
        try {
            actual = execute_operation(op, step, dao);
        } catch (const std::exception& error) {
            actual = error_json(error);
        }
        if (mask_clock &&
            (op == "getAllConversationsList" || op == "observeConversation" ||
             op == "getExecutionsForTask")) {
            if (actual.kind == json::Kind::Object) {
                for (json::Member& member : actual.members) {
                    if (member.key == "dataChangedAt") member.value = json_long(0);
                }
            } else if (actual.kind == json::Kind::Array) {
                for (json::Value& row : actual.elements) {
                    for (json::Member& member : row.members) {
                        if (member.key == "dataChangedAt") member.value = json_long(0);
                    }
                }
            }
        }
        const json::Value* expected = field(expected_step, "result");
        if (expected == nullptr || !json_equal(actual, *expected)) {
            scenario_fail(scenario_path.filename().string() + " step " + std::to_string(index) +
                          " (" + op + ")\n  expected: " +
                          (expected == nullptr ? std::string("<missing>") : excerpt(*expected)) +
                          "\n  actual:   " + excerpt(actual));
        }
    }
    const json::Value* expected_tables = field(trace, "tables");
    const json::Value actual_tables = dump_tables(sql, mask_clock);
    if (expected_tables == nullptr || !json_equal(actual_tables, *expected_tables)) {
        scenario_fail(scenario_path.filename().string() +
                      " table dump\n  expected: " +
                      (expected_tables == nullptr ? std::string("<missing>")
                                                  : excerpt(*expected_tables)) +
                      "\n  actual:   " + excerpt(actual_tables));
    }
}

void test_chat_dao_golden() {
    const std::filesystem::path root = AGORA_FIXTURE_CHAT_DAO_DIR;
    const std::filesystem::path scenario_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            scenarios.push_back(entry.path());
        }
    }
    std::sort(scenarios.begin(), scenarios.end());
    if (scenarios.empty()) scenario_fail("no chat-dao scenarios found");
    for (const std::filesystem::path& scenario : scenarios) {
        const std::filesystem::path trace = golden_dir / (scenario.stem().string() + ".trace");
        run_scenario(scenario, trace, root / "platform" / "schema.sql");
    }
}

json::Value reader_record_json(const export_snapshot::SnapshotRecord& record) {
    switch (record.type()) {
        case export_snapshot::RecordType::Conversation:
            return json_object({{"type", json_string("conversation")},
                                {"entity", conversation_json(std::get<ChatEntity>(record.payload))}});
        case export_snapshot::RecordType::Run:
            return json_object({{"type", json_string("run")},
                                {"entity", run_json(std::get<RunEntity>(record.payload))}});
        case export_snapshot::RecordType::Message:
            return json_object({{"type", json_string("message")},
                                {"entity", message_json(std::get<MessageEntity>(record.payload))}});
        case export_snapshot::RecordType::Loop:
            return json_object({{"type", json_string("loop")},
                                {"entity",
                                 loop_json(std::get<automation::LoopEntity>(record.payload))}});
        case export_snapshot::RecordType::Task:
            return json_object({{"type", json_string("task")},
                                {"entity",
                                 task_json(std::get<automation::TaskEntity>(record.payload))}});
    }
    throw KotlinError("IllegalStateException", "unknown snapshot record type");
}

json::Value reader_key_array(SqlExecutor& sql, const std::string& query) {
    std::vector<json::Value> keys;
    for (const SqlRow& row : sql.query(query, {})) {
        keys.push_back(json_string(row.at(0).as_text()));
    }
    return json_array(std::move(keys));
}

json::Value dump_reader_keys(SqlExecutor& sql) {
    return json_object({
        {"conversations", reader_key_array(sql, "SELECT id FROM conversations ORDER BY rowid")},
        {"runs", reader_key_array(sql, "SELECT id FROM runs ORDER BY rowid")},
        {"messages", reader_key_array(sql, "SELECT id FROM messages ORDER BY rowid")},
        {"loops", reader_key_array(sql, "SELECT conversationId FROM loops ORDER BY rowid")},
        {"tasks", reader_key_array(sql, "SELECT id FROM tasks ORDER BY rowid")},
    });
}

void run_reader_scenario(const std::filesystem::path& scenario_path,
                         const std::filesystem::path& trace_path,
                         const std::filesystem::path& schema_path) {
    const json::ParseResult scenario_parsed = json::parse(read_file(scenario_path));
    if (!scenario_parsed.value.has_value()) {
        scenario_fail("cannot parse " + scenario_path.string());
    }
    const json::ParseResult trace_parsed = json::parse(read_file(trace_path));
    if (!trace_parsed.value.has_value()) {
        scenario_fail("cannot parse " + trace_path.string());
    }
    const json::Value& scenario = *scenario_parsed.value;
    const json::Value& trace = *trace_parsed.value;

    SqliteExecutor sql(schema_path);
    if (const json::Value* seed = field(scenario, "seedSql")) {
        if (seed->kind != json::Kind::Array) {
            scenario_fail("seedSql is not an array in " + scenario_path.filename().string());
        }
        for (const json::Value& statement : seed->elements) {
            if (statement.kind != json::Kind::String) {
                scenario_fail("seedSql element is not a string");
            }
            sql.execute(statement.text, {});
        }
    }

    ChatDao dao(sql);
    export_snapshot::ChatDaoSnapshotStore store(dao, sql);

    const json::Value* scenario_steps = field(scenario, "steps");
    const json::Value* trace_steps = field(trace, "steps");
    if (scenario_steps == nullptr || trace_steps == nullptr ||
        scenario_steps->kind != json::Kind::Array || trace_steps->kind != json::Kind::Array) {
        scenario_fail("missing steps in " + scenario_path.filename().string());
    }
    if (scenario_steps->elements.size() != trace_steps->elements.size()) {
        scenario_fail("step count mismatch in " + scenario_path.filename().string());
    }

    for (std::size_t index = 0; index < scenario_steps->elements.size(); ++index) {
        const json::Value& step = scenario_steps->elements[index];
        const json::Value& expected_step = trace_steps->elements[index];
        const std::string op = required_string(step, "op");
        if (op != required_string(expected_step, "op")) {
            scenario_fail("reader trace op mismatch at step " + std::to_string(index));
        }

        const std::vector<std::string> skipped = string_list(step, "skipBody");
        const std::set<std::string> skip_set(skipped.begin(), skipped.end());
        const std::optional<std::string> fail_include =
            optional_string(step, "failIncludeBody");
        const std::optional<int> fail_at = optional_int(step, "failOnRecordAt");
        std::vector<json::Value> records;
        std::vector<json::Value> calls;
        json::Value actual;
        try {
            export_snapshot::read_snapshot(
                store,
                [&](const ChatEntity& conversation) {
                    calls.push_back(json_string(conversation.id));
                    if (fail_include.has_value() && *fail_include == conversation.id) {
                        throw KotlinError("IllegalStateException",
                                          "includeBody failed for " + conversation.id);
                    }
                    return skip_set.find(conversation.id) == skip_set.end();
                },
                [&](const export_snapshot::SnapshotRecord& record) {
                    if (fail_at.has_value() &&
                        static_cast<int>(records.size()) == *fail_at) {
                        throw KotlinError("IllegalStateException",
                                          "onRecord failed at " +
                                              std::to_string(records.size()));
                    }
                    records.push_back(reader_record_json(record));
                });
            actual = json_object({
                {"records", json_array(std::move(records))},
                {"includeBodyCalls", json_array(std::move(calls))},
            });
        } catch (const std::exception& error) {
            actual = error_json(error);
            actual.members.push_back({"records", json_array(std::move(records))});
            actual.members.push_back({"includeBodyCalls", json_array(std::move(calls))});
        }

        const json::Value* expected = field(expected_step, "result");
        if (expected == nullptr || !json_equal(actual, *expected)) {
            scenario_fail(scenario_path.filename().string() + " step " +
                          std::to_string(index) + " (" + op + ")\n  expected: " +
                          (expected == nullptr ? std::string("<missing>") : excerpt(*expected)) +
                          "\n  actual:   " + excerpt(actual));
        }
    }

    const json::Value* expected_tables = field(trace, "tables");
    const json::Value actual_tables = dump_reader_keys(sql);
    if (expected_tables == nullptr || !json_equal(actual_tables, *expected_tables)) {
        scenario_fail(scenario_path.filename().string() +
                      " table keys\n  expected: " +
                      (expected_tables == nullptr ? std::string("<missing>")
                                                  : excerpt(*expected_tables)) +
                      "\n  actual:   " + excerpt(actual_tables));
    }
}

void test_reader_golden() {
    const std::filesystem::path root = AGORA_FIXTURE_CONVERSATION_EXPORT_SNAPSHOT_READER_DIR;
    const std::filesystem::path scenario_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            scenarios.push_back(entry.path());
        }
    }
    std::sort(scenarios.begin(), scenarios.end());
    if (scenarios.empty()) scenario_fail("no snapshot-reader scenarios found");
    for (const std::filesystem::path& scenario : scenarios) {
        const std::filesystem::path trace = golden_dir / (scenario.stem().string() + ".trace");
        run_reader_scenario(scenario, trace, root / "platform" / "schema.sql");
    }
}

}  // namespace

TEST(chat_dao_golden_parity) { test_chat_dao_golden(); }
TEST(conversation_export_snapshot_reader_golden_parity) { test_reader_golden(); }
