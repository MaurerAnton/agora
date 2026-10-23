// Golden parity test for data/local/ChatSearchDao.kt.
//
// The stand runs the Kotlin interface against a real SQLite database through Room's replaced
// annotation processor; this test runs the C++ ChatSearchDao (which owns the transcribed SQL)
// against the same schema (platform/schema.sql, shipped with the stand). Each of the fourteen
// members is driven, and the run ends with a dump of the four observed tables — all of them
// unchanged, because the DAO is read-only.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include <sqlite3.h>

#include "agora/core/chat_search_dao.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace search = agora::core::chat_search;
namespace json = agora::core::json;
namespace recovery = agora::core::run_recovery;

using agora::core::ChatEntity;
using agora::core::KotlinError;
using agora::core::MessageEntity;
using agora::core::SqlArgs;
using agora::core::SqlError;
using agora::core::SqlRow;
using agora::core::SqlValue;
using search::ChatSearchDao;
using search::EmbeddingModelCount;
using search::EmbeddingSearchRow;
using search::IndexableMessage;

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

int optional_int(const json::Value& object, std::string_view name, int fallback) {
    const std::optional<std::int64_t> value = optional_long(object, name);
    return value.has_value() ? static_cast<int>(*value) : fallback;
}

std::vector<std::string> string_array(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr) scenario_fail(std::string(name) + " missing");
    if (value->kind != json::Kind::Array) scenario_fail(std::string(name) + " must be an array");
    for (const json::Value& element : value->elements) out.push_back(primitive_content(element));
    return out;
}

// ---- json builders ------------------------------------------------------------------------

json::Value json_null() {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = "null";
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

json::Value json_bool(bool flag) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = flag ? "true" : "false";
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

json::Value optional_json_long(const std::optional<std::int64_t>& number) {
    if (!number.has_value()) return json_null();
    return json_long(*number);
}

json::Value optional_json_int(const std::optional<int>& number) {
    if (!number.has_value()) return json_null();
    return json_long(*number);
}

std::string hex_lower(const std::vector<std::uint8_t>& bytes) {
    static constexpr char kDigits[] = "0123456789abcdef";
    std::string out;
    out.reserve(bytes.size() * 2);
    for (std::uint8_t byte : bytes) {
        out.push_back(kDigits[byte >> 4]);
        out.push_back(kDigits[byte & 0x0F]);
    }
    return out;
}

std::vector<std::uint8_t> hex_to_bytes(std::string_view hex) {
    if (hex.size() % 2 != 0) scenario_fail("hex text with an odd length");
    std::vector<std::uint8_t> out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const std::string byte_text(hex.substr(i, 2));
        std::size_t consumed = 0;
        const int parsed = std::stoi(byte_text, &consumed, 16);
        if (consumed != 2) scenario_fail("invalid hex text");
        out.push_back(static_cast<std::uint8_t>(parsed));
    }
    return out;
}

std::string message_status_to_wire(recovery::MessageStatus status) {
    switch (status) {
        case recovery::MessageStatus::transcribing:
            return "TRANSCRIBING";
        case recovery::MessageStatus::sending:
            return "SENDING";
        case recovery::MessageStatus::thinking:
            return "THINKING";
        case recovery::MessageStatus::tool_calling:
            return "TOOL_CALLING";
        case recovery::MessageStatus::success:
            return "SUCCESS";
        case recovery::MessageStatus::stopped:
            return "STOPPED";
        case recovery::MessageStatus::error:
            return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped MessageStatus");
}

std::string participant_to_wire(recovery::Participant participant) {
    switch (participant) {
        case recovery::Participant::user:
            return "USER";
        case recovery::Participant::model:
            return "MODEL";
        case recovery::Participant::error:
            return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped Participant");
}

// ---- sqlite executor (SqlExecutor boundary) -----------------------------------------------

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

// ---- seeding -------------------------------------------------------------------------------

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            const std::optional<std::string> branches =
                optional_string(row, "selectedBranchesJson");
            const std::optional<std::string> prompt = optional_string(row, "systemPromptId");
            const std::optional<std::string> model = optional_string(row, "modelId");
            const std::optional<std::string> task = optional_string(row, "taskId");
            const std::optional<std::string> draft = optional_string(row, "draftAttachments");
            const std::optional<std::string> run_branches =
                optional_string(row, "selectedRunBranchesJson");
            sql.execute(
                "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, "
                "selectedBranchesJson, systemPromptId, modelId, taskId, origin, graduated, "
                "draftText, draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, "
                "isPinned) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(required_string(row, "title")),
                 SqlValue::integer_value(required_long(row, "lastUpdated")),
                 SqlValue::integer_value(optional_long(row, "dataChangedAt").value_or(0)),
                 branches.has_value() ? SqlValue::text_value(*branches) : SqlValue::null(),
                 prompt.has_value() ? SqlValue::text_value(*prompt) : SqlValue::null(),
                 model.has_value() ? SqlValue::text_value(*model) : SqlValue::null(),
                 task.has_value() ? SqlValue::text_value(*task) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "origin").value_or("user")),
                 SqlValue::integer_value(
                     optional_string(row, "graduated").value_or("false") == "true" ? 1 : 0),
                 SqlValue::text_value(optional_string(row, "draftText").value_or("")),
                 draft.has_value() ? SqlValue::text_value(*draft) : SqlValue::null(),
                 run_branches.has_value() ? SqlValue::text_value(*run_branches)
                                          : SqlValue::null(),
                 SqlValue::integer_value(
                     optional_string(row, "hasUnreadGeneration").value_or("false") == "true" ? 1
                                                                                            : 0),
                 SqlValue::integer_value(
                     optional_string(row, "isPinned").value_or("false") == "true" ? 1 : 0)});
        }
    }
    if (const json::Value* runs = field(*seed_object, "runs"); runs != nullptr) {
        for (const json::Value& row : runs->elements) {
            const std::int64_t started_at = required_long(row, "startedAt");
            const std::optional<std::string> parent = optional_string(row, "parentRunId");
            const std::optional<std::int64_t> slot = optional_long(row, "activeSlot");
            const std::optional<std::int64_t> stopped = optional_long(row, "stopRequestedAt");
            const std::optional<std::int64_t> ended = optional_long(row, "endedAt");
            const std::optional<std::string> reason = optional_string(row, "endReason");
            sql.execute(
                "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
                "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                "legacyAmbiguous) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(required_string(row, "conversationId")),
                 parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null(),
                 SqlValue::text_value(required_string(row, "status")),
                 slot.has_value() ? SqlValue::integer_value(*slot) : SqlValue::null(),
                 SqlValue::integer_value(started_at),
                 SqlValue::integer_value(
                     optional_long(row, "lastCheckpointAt").value_or(started_at)),
                 stopped.has_value() ? SqlValue::integer_value(*stopped) : SqlValue::null(),
                 ended.has_value() ? SqlValue::integer_value(*ended) : SqlValue::null(),
                 reason.has_value() ? SqlValue::text_value(*reason) : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "currentPass").value_or(0)),
                 SqlValue::integer_value(
                     optional_string(row, "legacyAmbiguous").value_or("false") == "true" ? 1 : 0)});
        }
    }
    if (const json::Value* messages = field(*seed_object, "messages"); messages != nullptr) {
        for (const json::Value& row : messages->elements) {
            std::string images;
            if (const json::Value* list = field(row, "images");
                list != nullptr && list->kind == json::Kind::Array) {
                images = json::to_kotlin_string(*list);
            } else if (const std::optional<std::string> raw = optional_string(row, "imagesRaw");
                       raw.has_value()) {
                images = *raw;
            } else {
                images = "[]";
            }
            SqlArgs args;
            args.push_back(SqlValue::text_value(required_string(row, "id")));
            args.push_back(SqlValue::text_value(required_string(row, "conversationId")));
            const std::optional<std::string> parent = optional_string(row, "parentId");
            args.push_back(parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null());
            args.push_back(SqlValue::text_value(optional_string(row, "text").value_or("")));
            args.push_back(SqlValue::text_value(images));
            const std::optional<std::string> thoughts = optional_string(row, "thoughts");
            args.push_back(thoughts.has_value() ? SqlValue::text_value(*thoughts)
                                                : SqlValue::null());
            const std::optional<std::string> thought_title = optional_string(row, "thoughtTitle");
            args.push_back(thought_title.has_value() ? SqlValue::text_value(*thought_title)
                                                     : SqlValue::null());
            args.push_back(SqlValue::integer_value(optional_long(row, "tokenCount").value_or(0)));
            for (const char* name : {"inputTokenCount", "cachedInputTokenCount",
                                     "cacheWriteInputTokenCount", "uncachedInputTokenCount",
                                     "outputTokenCount", "reasoningTokenCount"}) {
                const std::optional<std::int64_t> value = optional_long(row, name);
                args.push_back(value.has_value() ? SqlValue::integer_value(*value)
                                                 : SqlValue::null());
            }
            const std::optional<std::int64_t> duration = optional_long(row, "generationDurationMs");
            args.push_back(duration.has_value() ? SqlValue::integer_value(*duration)
                                                : SqlValue::null());
            args.push_back(SqlValue::text_value(
                optional_string(row, "status").value_or("SUCCESS")));
            args.push_back(SqlValue::text_value(
                optional_string(row, "participant").value_or("MODEL")));
            args.push_back(SqlValue::integer_value(required_long(row, "timestamp")));
            const std::optional<std::int64_t> thought_time = optional_long(row, "thoughtTimeMs");
            args.push_back(thought_time.has_value() ? SqlValue::integer_value(*thought_time)
                                                    : SqlValue::null());
            const std::optional<std::string> model = optional_string(row, "modelName");
            args.push_back(model.has_value() ? SqlValue::text_value(*model) : SqlValue::null());
            const std::optional<std::string> tool_json = optional_string(row, "toolCallJson");
            args.push_back(tool_json.has_value() ? SqlValue::text_value(*tool_json)
                                                 : SqlValue::null());
            const std::optional<std::string> attachment = optional_string(row, "attachmentMeta");
            args.push_back(attachment.has_value() ? SqlValue::text_value(*attachment)
                                                  : SqlValue::null());
            args.push_back(SqlValue::text_value(required_string(row, "runId")));
            args.push_back(SqlValue::integer_value(optional_long(row, "runSequence").value_or(-1)));
            const std::optional<std::int64_t> consumed = optional_long(row, "consumedAtPass");
            args.push_back(consumed.has_value() ? SqlValue::integer_value(*consumed)
                                                : SqlValue::null());
            const std::optional<std::string> source = optional_string(row, "sourceJson");
            args.push_back(source.has_value() ? SqlValue::text_value(*source) : SqlValue::null());
            sql.execute(
                "INSERT INTO messages (id, conversationId, parentId, text, images, thoughts, "
                "thoughtTitle, tokenCount, inputTokenCount, cachedInputTokenCount, "
                "cacheWriteInputTokenCount, uncachedInputTokenCount, outputTokenCount, "
                "reasoningTokenCount, generationDurationMs, status, participant, timestamp, "
                "thoughtTimeMs, modelName, toolCallJson, attachmentMeta, runId, runSequence, "
                "consumedAtPass, sourceJson) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, "
                "?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                args);
        }
    }
    if (const json::Value* embeddings = field(*seed_object, "embeddings");
        embeddings != nullptr) {
        for (const json::Value& row : embeddings->elements) {
            SqlValue blob;
            blob.kind = SqlValue::Kind::Blob;
            blob.blob = hex_to_bytes(required_string(row, "embedding"));
            const std::optional<std::int64_t> id = optional_long(row, "id");
            const std::optional<std::string> fingerprint = optional_string(row, "sourceFingerprint");
            sql.execute(
                "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
                "sourceFingerprint) VALUES (?, ?, ?, ?, ?, ?, ?)",
                {id.has_value() ? SqlValue::integer_value(*id) : SqlValue::null(),
                 SqlValue::text_value(required_string(row, "messageId")),
                 SqlValue::text_value(required_string(row, "modelId")), std::move(blob),
                 SqlValue::text_value(optional_string(row, "chunkText").value_or("")),
                 SqlValue::integer_value(optional_long(row, "dimension").value_or(0)),
                 fingerprint.has_value() ? SqlValue::text_value(*fingerprint) : SqlValue::null()});
        }
    }
}

// ---- rendering ----------------------------------------------------------------------------

json::Value message_json(const MessageEntity& entity) {
    std::vector<json::Value> images;
    for (const std::string& image : entity.images) images.push_back(json_string(image));
    return json_object({{"id", json_string(entity.id)},
                        {"conversationId", json_string(entity.conversation_id)},
                        {"parentId", optional_json_string(entity.parent_id)},
                        {"text", json_string(entity.text)},
                        {"images", json_array(std::move(images))},
                        {"thoughts", optional_json_string(entity.thoughts)},
                        {"thoughtTitle", optional_json_string(entity.thought_title)},
                        {"tokenCount", json_long(entity.token_count)},
                        {"inputTokenCount", optional_json_int(entity.input_token_count)},
                        {"cachedInputTokenCount", optional_json_int(entity.cached_input_token_count)},
                        {"cacheWriteInputTokenCount",
                         optional_json_int(entity.cache_write_input_token_count)},
                        {"uncachedInputTokenCount",
                         optional_json_int(entity.uncached_input_token_count)},
                        {"outputTokenCount", optional_json_int(entity.output_token_count)},
                        {"reasoningTokenCount", optional_json_int(entity.reasoning_token_count)},
                        {"generationDurationMs", optional_json_long(entity.generation_duration_ms)},
                        {"status", json_string(message_status_to_wire(entity.status))},
                        {"participant", json_string(participant_to_wire(entity.participant))},
                        {"timestamp", json_long(entity.timestamp)},
                        {"thoughtTimeMs", optional_json_long(entity.thought_time_ms)},
                        {"modelName", optional_json_string(entity.model_name)},
                        {"toolCallJson", optional_json_string(entity.tool_call_json)},
                        {"attachmentMeta", optional_json_string(entity.attachment_meta)},
                        {"runId", json_string(entity.run_id)},
                        {"runSequence", json_long(entity.run_sequence)},
                        {"consumedAtPass", optional_json_int(entity.consumed_at_pass)},
                        {"sourceJson", optional_json_string(entity.source_json)}});
}

json::Value conversation_json(const ChatEntity& entity) {
    return json_object({{"id", json_string(entity.id)},
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
                        {"selectedRunBranchesJson",
                         optional_json_string(entity.selected_run_branches_json)},
                        {"hasUnreadGeneration", json_bool(entity.has_unread_generation)},
                        {"isPinned", json_bool(entity.is_pinned)}});
}

json::Value embedding_search_row_json(const EmbeddingSearchRow& entity) {
    return json_object({{"id", json_long(entity.id)},
                        {"messageId", json_string(entity.message_id)},
                        {"embedding", json_string(hex_lower(entity.embedding))},
                        {"dimension", json_long(entity.dimension)}});
}

json::Value embedding_model_count_json(const EmbeddingModelCount& entity) {
    return json_object(
        {{"modelId", json_string(entity.model_id)}, {"count", json_long(entity.count)}});
}

json::Value indexable_message_json(const IndexableMessage& entity) {
    return json_object(
        {{"id", json_string(entity.id)}, {"text", json_string(entity.text)}});
}

json::Value message_array(const std::vector<MessageEntity>& messages) {
    std::vector<json::Value> out;
    for (const MessageEntity& message : messages) out.push_back(message_json(message));
    return json_array(std::move(out));
}

json::Value conversation_array(const std::vector<ChatEntity>& conversations) {
    std::vector<json::Value> out;
    for (const ChatEntity& conversation : conversations) {
        out.push_back(conversation_json(conversation));
    }
    return json_array(std::move(out));
}

json::Value indexable_array(const std::vector<IndexableMessage>& messages) {
    std::vector<json::Value> out;
    for (const IndexableMessage& message : messages) out.push_back(indexable_message_json(message));
    return json_array(std::move(out));
}

// ---- driver --------------------------------------------------------------------------------

json::Value dispatch(const std::string& op, const json::Value& step, ChatSearchDao& dao) {
    if (op == "searchMessages") {
        return message_array(dao.search_messages(
            required_string(step, "query"), optional_int(step, "limit", 10)));
    }
    if (op == "getMessagesWithCitationSegmentsPage") {
        return message_array(dao.get_messages_with_citation_segments_page(
            required_string(step, "afterId"), required_int(step, "pageSize")));
    }
    if (op == "getEmbeddingSearchPage") {
        const std::vector<EmbeddingSearchRow> rows = dao.get_embedding_search_page(
            required_string(step, "modelId"), required_long(step, "afterId"),
            required_int(step, "minimumTextLength"), required_int(step, "limit"));
        std::vector<json::Value> out;
        for (const EmbeddingSearchRow& row : rows) out.push_back(embedding_search_row_json(row));
        return json_array(std::move(out));
    }
    if (op == "getEmbeddingCountByModel") {
        return json_long(dao.get_embedding_count_by_model(required_string(step, "modelId")));
    }
    if (op == "getEmbeddingCountsByModels") {
        const std::vector<EmbeddingModelCount> rows =
            dao.get_embedding_counts_by_models(string_array(step, "modelIds"));
        std::vector<json::Value> out;
        for (const EmbeddingModelCount& row : rows) out.push_back(embedding_model_count_json(row));
        return json_array(std::move(out));
    }
    if (op == "getIndexableMessageCount") {
        return json_long(dao.get_indexable_message_count());
    }
    if (op == "getSearchableMessagesPage") {
        return indexable_array(dao.get_searchable_messages_page(
            optional_string(step, "afterId"), required_int(step, "limit")));
    }
    if (op == "getUnembeddedMessagesPage") {
        return indexable_array(dao.get_unembedded_messages_page(
            required_string(step, "modelId"), optional_string(step, "afterId"),
            required_int(step, "limit")));
    }
    if (op == "getSearchableMessagesByIds") {
        return message_array(dao.get_searchable_messages_by_ids(string_array(step, "ids")));
    }
    if (op == "isMessageSearchable") {
        return json_bool(dao.is_message_searchable(required_string(step, "messageId")));
    }
    if (op == "getSearchableConversation") {
        const std::optional<ChatEntity> conversation =
            dao.get_searchable_conversation(required_string(step, "conversationId"));
        if (!conversation.has_value()) return json_null();
        return conversation_json(*conversation);
    }
    if (op == "getSearchableConversationCount") {
        return json_long(dao.get_searchable_conversation_count());
    }
    if (op == "getSearchableConversationsPageAscending") {
        return conversation_array(dao.get_searchable_conversations_page_ascending(
            required_int(step, "offset"), required_int(step, "limit")));
    }
    if (op == "getSearchableConversationsPageDescending") {
        return conversation_array(dao.get_searchable_conversations_page_descending(
            required_int(step, "offset"), required_int(step, "limit")));
    }
    throw KotlinError("IllegalStateException", "unknown operation " + op);
}

// ---- table dump ----------------------------------------------------------------------------

json::Value query_rows(SqliteExecutor& sql, const std::string& query,
                       json::Value (*render)(const SqlRow&)) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(query, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value conversation_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"title", json_string(row[1].as_text())},
                        {"lastUpdated", json_long(row[2].as_integer())},
                        {"dataChangedAt", json_long(row[3].as_integer())},
                        {"selectedBranchesJson", optional_json_string(row[4].text_or_null())},
                        {"systemPromptId", optional_json_string(row[5].text_or_null())},
                        {"modelId", optional_json_string(row[6].text_or_null())},
                        {"taskId", optional_json_string(row[7].text_or_null())},
                        {"origin", json_string(row[8].as_text())},
                        {"graduated", json_bool(row[9].as_integer() != 0)},
                        {"draftText", json_string(row[10].as_text())},
                        {"draftAttachments", optional_json_string(row[11].text_or_null())},
                        {"selectedRunBranchesJson", optional_json_string(row[12].text_or_null())},
                        {"hasUnreadGeneration", json_bool(row[13].as_integer() != 0)},
                        {"isPinned", json_bool(row[14].as_integer() != 0)}});
}

json::Value run_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"conversationId", json_string(row[1].as_text())},
                        {"parentRunId", optional_json_string(row[2].text_or_null())},
                        {"status", json_string(row[3].as_text())},
                        {"activeSlot", optional_json_long(row[4].integer_or_null())},
                        {"startedAt", json_long(row[5].as_integer())},
                        {"lastCheckpointAt", json_long(row[6].as_integer())},
                        {"stopRequestedAt", optional_json_long(row[7].integer_or_null())},
                        {"endedAt", optional_json_long(row[8].integer_or_null())},
                        {"endReason", optional_json_string(row[9].text_or_null())},
                        {"currentPass", json_long(row[10].as_integer())},
                        {"legacyAmbiguous", json_bool(row[11].as_integer() != 0)}});
}

json::Value message_row_json(const SqlRow& row) {
    return json_object({{"id", json_string(row[0].as_text())},
                        {"conversationId", json_string(row[1].as_text())},
                        {"parentId", optional_json_string(row[2].text_or_null())},
                        {"text", json_string(row[3].as_text())},
                        {"images", json_string(row[4].as_text())},
                        {"thoughts", optional_json_string(row[5].text_or_null())},
                        {"thoughtTitle", optional_json_string(row[6].text_or_null())},
                        {"tokenCount", json_long(row[7].as_integer())},
                        {"inputTokenCount", optional_json_long(row[8].integer_or_null())},
                        {"cachedInputTokenCount", optional_json_long(row[9].integer_or_null())},
                        {"cacheWriteInputTokenCount", optional_json_long(row[10].integer_or_null())},
                        {"uncachedInputTokenCount", optional_json_long(row[11].integer_or_null())},
                        {"outputTokenCount", optional_json_long(row[12].integer_or_null())},
                        {"reasoningTokenCount", optional_json_long(row[13].integer_or_null())},
                        {"generationDurationMs", optional_json_long(row[14].integer_or_null())},
                        {"status", json_string(row[15].as_text())},
                        {"participant", json_string(row[16].as_text())},
                        {"timestamp", json_long(row[17].as_integer())},
                        {"thoughtTimeMs", optional_json_long(row[18].integer_or_null())},
                        {"modelName", optional_json_string(row[19].text_or_null())},
                        {"toolCallJson", optional_json_string(row[20].text_or_null())},
                        {"attachmentMeta", optional_json_string(row[21].text_or_null())},
                        {"runId", json_string(row[22].as_text())},
                        {"runSequence", json_long(row[23].as_integer())},
                        {"consumedAtPass", optional_json_long(row[24].integer_or_null())},
                        {"sourceJson", optional_json_string(row[25].text_or_null())}});
}

json::Value embedding_row_json(const SqlRow& row) {
    return json_object({{"id", json_long(row[0].as_integer())},
                        {"messageId", json_string(row[1].as_text())},
                        {"modelId", json_string(row[2].as_text())},
                        {"embedding", json_string(hex_lower(row[3].as_blob()))},
                        {"chunkText", json_string(row[4].as_text())},
                        {"dimension", json_long(row[5].as_integer())},
                        {"sourceFingerprint", optional_json_string(row[6].text_or_null())}});
}

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"conversations",
          query_rows(sql,
                     "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, "
                     "systemPromptId, modelId, taskId, origin, graduated, draftText, "
                     "draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, isPinned "
                     "FROM conversations ORDER BY id",
                     conversation_row_json)},
         {"runs",
          query_rows(sql,
                     "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, "
                     "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                     "legacyAmbiguous FROM runs ORDER BY id",
                     run_row_json)},
         {"messages",
          query_rows(sql,
                     "SELECT id, conversationId, parentId, text, images, thoughts, thoughtTitle, "
                     "tokenCount, inputTokenCount, cachedInputTokenCount, "
                     "cacheWriteInputTokenCount, uncachedInputTokenCount, outputTokenCount, "
                     "reasoningTokenCount, generationDurationMs, status, participant, timestamp, "
                     "thoughtTimeMs, modelName, toolCallJson, attachmentMeta, runId, runSequence, "
                     "consumedAtPass, sourceJson FROM messages ORDER BY id",
                     message_row_json)},
         {"embeddings",
          query_rows(sql,
                     "SELECT id, messageId, modelId, embedding, chunkText, dimension, "
                     "sourceFingerprint FROM embeddings ORDER BY id",
                     embedding_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_CHAT_SEARCH_DAO_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    ChatSearchDao dao(sql);

    std::vector<json::Value> steps;
    const json::Value* step_list = field(root, "steps");
    if (step_list != nullptr && step_list->kind == json::Kind::Array) {
        for (const json::Value& element : step_list->elements) {
            const std::string op = required_string(element, "op");
            json::Value result;
            try {
                result = dispatch(op, element, dao);
            } catch (const KotlinError& failure) {
                result = json_object({{"error", json_string(failure.class_name())},
                                      {"message", json_string(failure.what())}});
            } catch (const SqlError& failure) {
                result = json_object({{"error", json_string("SQLException")},
                                      {"message", json_string(failure.what())}});
            }
            steps.push_back(json_object({{"op", json_string(op)}, {"result", std::move(result)}}));
        }
    }

    json::Value trace = json_object({{"steps", json_array(std::move(steps))},
                                     {"tables", dump_tables(sql)}});
    return json::to_kotlin_string(trace);
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

TEST(chat_search_dao_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CHAT_SEARCH_DAO_DIR;
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
