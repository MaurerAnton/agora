// Golden parity test for the runtime recovery body (data/local/ChatDaoRuntimeRecovery.kt).
//
// The stand runs the Kotlin body against a real SQLite database through Room's replaced
// annotation processor; this test runs the C++ port against the same schema (platform/schema.sql,
// shipped with the stand) through SqliteRecoveryStore, which transcribes the nine ChatDao queries
// the stand's platform/ChatDao.kt declares. Each step records its result (or the Kotlin
// validation error it raises) and the run ends with a dump of the three observed tables.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include <sqlite3.h>

#include "agora/core/json.hpp"
#include "agora/core/runtime_recovery.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace recovery = agora::core::runtime_recovery;
namespace run_recovery = agora::core::run_recovery;
namespace conversation = agora::core::conversation_runtime;

using agora::core::KotlinError;
using agora::core::kotlin_check;
using agora::core::kotlin_require;
using agora::core::SqlArgs;
using agora::core::SqlError;
using agora::core::SqlRow;
using agora::core::SqlValue;
using agora::core::TransactionStack;

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

// ---- enum + converter boundaries -----------------------------------------------------------

/** Enum.valueOf semantics with Java's exact failure text. */
std::string enum_value(std::string_view name, std::string_view canonical_class,
                       std::initializer_list<std::pair<std::string_view, std::string_view>> table) {
    for (const auto& [wire, mapped] : table) {
        if (wire == name) return std::string(mapped);
    }
    throw KotlinError("IllegalArgumentException",
                      "No enum constant " + std::string(canonical_class) + "." + std::string(name));
}

/** MessageConverters.toStringList: JSON array, legacy "|||" delimiter accepted. */
std::vector<std::string> decode_string_list(const std::string& raw) {
    if (raw.empty()) return {};
    const json::ParseResult parsed = json::parse(raw);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Array) {
        std::vector<std::string> out;
        for (const json::Value& element : parsed.value->elements) {
            if (element.kind != json::Kind::String) scenario_fail("images must hold strings");
            out.push_back(element.text);
        }
        return out;
    }
    // Legacy format: "|||" delimiter.
    std::vector<std::string> parts;
    std::size_t start = 0;
    for (;;) {
        const std::size_t at = raw.find("|||", start);
        if (at == std::string::npos) {
            parts.push_back(raw.substr(start));
            break;
        }
        parts.push_back(raw.substr(start, at - start));
        start = at + 3;
    }
    return parts;
}

/** MessageConverters.fromStringList: the JSON array encoding of a list. */
std::string encode_string_list(const std::vector<std::string>& values) {
    json::Value array = json_array([&] {
        std::vector<json::Value> out;
        for (const std::string& value : values) out.push_back(json_string(value));
        return out;
    }());
    return json::to_kotlin_string(array);
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
        if (rc == SQLITE_ROW) {
            // java.sql.SQLException("Query returns results").
            sqlite3_finalize(statement);
            throw SqlError("Query returns results");
        }
        const std::string message = sqlite3_errmsg(db_);
        sqlite3_finalize(statement);
        if ((rc & 0xFF) == SQLITE_CONSTRAINT) throw agora::core::SqlConstraintError(message);
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
                    const int size = sqlite3_column_bytes(statement, i);
                    value.text.assign(reinterpret_cast<const char*>(text),
                                      static_cast<std::size_t>(size));
                    break;
                }
                case SQLITE_BLOB: {
                    value.kind = SqlValue::Kind::Blob;
                    const auto* bytes = sqlite3_column_blob(statement, i);
                    const int size = sqlite3_column_bytes(statement, i);
                    const auto* first = static_cast<const std::uint8_t*>(bytes);
                    value.blob.assign(first, first + size);
                    break;
                }
                default:
                    throw SqlError("sqlite: unsupported column type");
            }
            row.push_back(std::move(value));
        }
        return row;
    }

    void exec_raw(const char* sql) {
        char* error = nullptr;
        if (sqlite3_exec(db_, sql, nullptr, nullptr, &error) != SQLITE_OK) {
            const std::string message = error != nullptr ? error : "statement failed";
            sqlite3_free(error);
            throw SqlError("sqlite: " + message);
        }
    }

    sqlite3* db_ = nullptr;
};

// ---- the ChatDao store (the nine transcribed queries) -------------------------------------

/** RunEntity's init rules (data/local/RunEntity.kt), applied when a row is mapped. */
struct RunRow {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    std::string status;
    std::optional<std::int64_t> active_slot;
    std::int64_t started_at = 0;
    std::int64_t last_checkpoint_at = 0;
    std::optional<std::int64_t> stop_requested_at;
    std::optional<std::int64_t> ended_at;
    std::optional<std::string> end_reason;
    std::int64_t current_pass = 0;
    bool legacy_ambiguous = false;

    bool terminal() const {
        return status == "COMPLETED" || status == "STOPPED" || status == "FAILED";
    }

    void validate() const {
        kotlin_require(current_pass >= 0);
        kotlin_require((active_slot.has_value() && *active_slot == 1) == !terminal());
        kotlin_require(terminal() == (ended_at.has_value() && end_reason.has_value()));
        kotlin_require(status != "STOPPING" || stop_requested_at.has_value());
    }
};

conversation::RunStatus run_status_from_wire(const std::string& wire) {
    if (wire == "ACTIVE") return conversation::RunStatus::Active;
    if (wire == "STOPPING") return conversation::RunStatus::Stopping;
    if (wire == "COMPLETED") return conversation::RunStatus::Completed;
    if (wire == "STOPPED") return conversation::RunStatus::Stopped;
    if (wire == "FAILED") return conversation::RunStatus::Failed;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.RunStatus." + wire);
}

std::string run_status_to_wire(conversation::RunStatus status) {
    switch (status) {
        case conversation::RunStatus::Active:
            return "ACTIVE";
        case conversation::RunStatus::Stopping:
            return "STOPPING";
        case conversation::RunStatus::Completed:
            return "COMPLETED";
        case conversation::RunStatus::Stopped:
            return "STOPPED";
        case conversation::RunStatus::Failed:
            return "FAILED";
    }
    throw KotlinError("IllegalArgumentException", "unmapped RunStatus");
}

std::string run_end_reason_to_wire(conversation::RunEndReason reason) {
    switch (reason) {
        case conversation::RunEndReason::ModelCompleted:
            return "MODEL_COMPLETED";
        case conversation::RunEndReason::UserStopped:
            return "USER_STOPPED";
        case conversation::RunEndReason::ProcessRecovered:
            return "PROCESS_RECOVERED";
        case conversation::RunEndReason::ProviderError:
            return "PROVIDER_ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped RunEndReason");
}

run_recovery::MessageStatus message_status_from_wire(const std::string& wire) {
    if (wire == "TRANSCRIBING") return run_recovery::MessageStatus::transcribing;
    if (wire == "SENDING") return run_recovery::MessageStatus::sending;
    if (wire == "THINKING") return run_recovery::MessageStatus::thinking;
    if (wire == "TOOL_CALLING") return run_recovery::MessageStatus::tool_calling;
    if (wire == "SUCCESS") return run_recovery::MessageStatus::success;
    if (wire == "STOPPED") return run_recovery::MessageStatus::stopped;
    if (wire == "ERROR") return run_recovery::MessageStatus::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.MessageStatus." + wire);
}

std::string message_status_to_wire(run_recovery::MessageStatus status) {
    switch (status) {
        case run_recovery::MessageStatus::transcribing:
            return "TRANSCRIBING";
        case run_recovery::MessageStatus::sending:
            return "SENDING";
        case run_recovery::MessageStatus::thinking:
            return "THINKING";
        case run_recovery::MessageStatus::tool_calling:
            return "TOOL_CALLING";
        case run_recovery::MessageStatus::success:
            return "SUCCESS";
        case run_recovery::MessageStatus::stopped:
            return "STOPPED";
        case run_recovery::MessageStatus::error:
            return "ERROR";
    }
    throw KotlinError("IllegalArgumentException", "unmapped MessageStatus");
}

run_recovery::Participant participant_from_wire(const std::string& wire) {
    if (wire == "USER") return run_recovery::Participant::user;
    if (wire == "MODEL") return run_recovery::Participant::model;
    if (wire == "ERROR") return run_recovery::Participant::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.Participant." + wire);
}

RunRow map_run_row(const SqlRow& row) {
    // `SELECT * FROM runs` over the stand schema: constructor arguments convert first
    // (Enum.valueOf), then the init block runs — Kotlin's mapping order.
    kotlin_check(row.size() == 12, "RunEntity: row shape mismatch");
    RunRow run;
    run.id = row[0].as_text();
    run.conversation_id = row[1].as_text();
    run.parent_run_id = row[2].text_or_null();
    run.status = [&] {
        const std::string& value = row[3].as_text();
        if (value == "ACTIVE" || value == "STOPPING" || value == "COMPLETED" ||
            value == "STOPPED" || value == "FAILED") {
            return value;
        }
        throw KotlinError("IllegalArgumentException",
                          "No enum constant com.newoether.agora.model.RunStatus." + value);
    }();
    run.active_slot = row[4].integer_or_null();
    run.started_at = row[5].as_integer();
    run.last_checkpoint_at = row[6].as_integer();
    run.stop_requested_at = row[7].integer_or_null();
    run.ended_at = row[8].integer_or_null();
    run.end_reason = row[9].text_or_null();
    if (run.end_reason.has_value()) {
        const std::string& value = *run.end_reason;
        if (value != "MODEL_COMPLETED" && value != "USER_STOPPED" &&
            value != "PROCESS_RECOVERED" && value != "PROVIDER_ERROR") {
            throw KotlinError("IllegalArgumentException",
                              "No enum constant com.newoether.agora.model.RunEndReason." + value);
        }
    }
    run.current_pass = row[10].as_integer();
    run.legacy_ambiguous = row[11].as_integer() != 0;
    run.validate();
    return run;
}

class SqliteRecoveryStore : public recovery::RecoveryStore {
public:
    explicit SqliteRecoveryStore(SqliteExecutor& sql) : sql_(sql), stack_(sql) {}

    void begin_transaction() override { stack_.begin(); }
    void set_transaction_successful() override { stack_.mark_successful(); }
    void end_transaction() override { stack_.end(); }

    std::optional<recovery::ConversationSnapshot> get_conversation(
        std::string_view conversation_id) override {
        // ChatContextCompactDao.getConversation: SELECT * FROM conversations WHERE id = :conversationId
        std::vector<SqlRow> rows = sql_.query(
            "SELECT * FROM conversations WHERE id = ?",
            {SqlValue::text_value(std::string(conversation_id))});
        if (rows.empty()) return std::nullopt;
        kotlin_check(rows.front().size() >= 2, "ChatEntity: row shape mismatch");
        recovery::ConversationSnapshot snapshot;
        snapshot.id = rows.front()[0].as_text();
        snapshot.selected_run_branches_json = rows.front()[1].text_or_null();
        return snapshot;
    }

    std::optional<recovery::RecoveryRun> get_live_run(std::string_view conversation_id) override {
        // ChatContextCompactDao.getLiveRun
        std::vector<SqlRow> rows = sql_.query(
            "SELECT * FROM runs WHERE conversationId = ? AND activeSlot = 1 LIMIT 1",
            {SqlValue::text_value(std::string(conversation_id))});
        if (rows.empty()) return std::nullopt;
        const RunRow run = map_run_row(rows.front());
        recovery::RecoveryRun live;
        live.id = run.id;
        live.status = run_status_from_wire(run.status);
        live.current_pass = static_cast<int>(run.current_pass);
        return live;
    }

    std::vector<recovery::RecoveryMessage> get_messages_for_runs(
        const std::vector<std::string>& run_ids) override {
        // ChatContextCompactDao.getMessagesForRuns
        if (run_ids.empty()) return {};
        std::string placeholders;
        SqlArgs args;
        for (std::size_t i = 0; i < run_ids.size(); ++i) {
            if (i != 0) placeholders.push_back(',');
            placeholders.push_back('?');
            args.push_back(SqlValue::text_value(run_ids[i]));
        }
        const std::string sql =
            "SELECT * FROM messages WHERE runId IN (" + placeholders +
            ") ORDER BY runSequence, timestamp, id";
        std::vector<recovery::RecoveryMessage> out;
        for (const SqlRow& row : sql_.query(sql, args)) {
            out.push_back(map_message_row(row));
        }
        return out;
    }

    std::int64_t update_message_checkpoint(
        const recovery::MessageCheckpoint& checkpoint) override {
        // ChatDao.updateMessageCheckpoint: `@Update(entity = MessageEntity::class)` — SET every
        // projection field that names an entity column, WHERE the primary key.
        static constexpr std::string_view kSql =
            "UPDATE messages SET text = ?, images = ?, thoughts = ?, thoughtTitle = ?, "
            "tokenCount = ?, inputTokenCount = ?, cachedInputTokenCount = ?, "
            "cacheWriteInputTokenCount = ?, uncachedInputTokenCount = ?, outputTokenCount = ?, "
            "reasoningTokenCount = ?, generationDurationMs = ?, status = ?, thoughtTimeMs = ?, "
            "toolCallJson = ? WHERE id = ?";
        SqlArgs args;
        args.push_back(SqlValue::text_value(checkpoint.text));
        args.push_back(SqlValue::text_value(encode_string_list(checkpoint.images)));
        args.push_back(opt(checkpoint.thoughts));
        args.push_back(opt(checkpoint.thought_title));
        args.push_back(SqlValue::integer_value(checkpoint.token_count));
        args.push_back(opt(checkpoint.input_token_count));
        args.push_back(opt(checkpoint.cached_input_token_count));
        args.push_back(opt(checkpoint.cache_write_input_token_count));
        args.push_back(opt(checkpoint.uncached_input_token_count));
        args.push_back(opt(checkpoint.output_token_count));
        args.push_back(opt(checkpoint.reasoning_token_count));
        args.push_back(opt(checkpoint.generation_duration_ms));
        args.push_back(SqlValue::text_value(message_status_to_wire(checkpoint.status)));
        args.push_back(opt(checkpoint.thought_time_ms));
        args.push_back(opt(checkpoint.tool_call_json));
        args.push_back(SqlValue::text_value(checkpoint.id));
        return sql_.execute(std::string(kSql), args);
    }

    std::int64_t terminalize_live_run(std::string_view run_id, conversation::RunStatus status,
                                      conversation::RunEndReason reason,
                                      std::int64_t at) override {
        static constexpr std::string_view kSql =
            "UPDATE runs SET status = ?, activeSlot = NULL, lastCheckpointAt = ?, endedAt = ?, "
            "endReason = ? WHERE id = ? AND activeSlot = 1";
        return sql_.execute(std::string(kSql),
                            {SqlValue::text_value(run_status_to_wire(status)),
                             SqlValue::integer_value(at), SqlValue::integer_value(at),
                             SqlValue::text_value(run_end_reason_to_wire(reason)),
                             SqlValue::text_value(std::string(run_id))});
    }

    std::int64_t compare_and_set_run_branch_selections(std::string_view conversation_id,
                                                      std::string_view expected,
                                                      std::string_view replacement) override {
        static constexpr std::string_view kSql =
            "UPDATE conversations SET selectedRunBranchesJson = ? "
            "WHERE id = ? AND selectedRunBranchesJson = ?";
        return sql_.execute(std::string(kSql),
                            {SqlValue::text_value(std::string(replacement)),
                             SqlValue::text_value(std::string(conversation_id)),
                             SqlValue::text_value(std::string(expected))});
    }

    std::int64_t stop_stuck_messages_for_conversation(std::string_view conversation_id) override {
        static constexpr std::string_view kSql =
            "UPDATE messages SET status = 'STOPPED' WHERE conversationId = ? AND status IN "
            "('SENDING', 'THINKING', 'TOOL_CALLING', 'TRANSCRIBING')";
        return sql_.execute(std::string(kSql),
                            {SqlValue::text_value(std::string(conversation_id))});
    }

    std::int64_t touch_conversation_data(std::string_view conversation_id,
                                        std::int64_t at) override {
        static constexpr std::string_view kSql =
            "UPDATE conversations SET dataChangedAt = MAX(dataChangedAt + 1, ?) WHERE id = ?";
        return sql_.execute(std::string(kSql),
                            {SqlValue::integer_value(at),
                             SqlValue::text_value(std::string(conversation_id))});
    }

    std::vector<agora::core::run_branch_selection::RunRef> get_runs_for_conversation_snapshot(
        std::string_view conversation_id) override {
        std::vector<agora::core::run_branch_selection::RunRef> out;
        for (const SqlRow& row : sql_.query(
                 "SELECT * FROM runs WHERE conversationId = ? ORDER BY startedAt, id",
                 {SqlValue::text_value(std::string(conversation_id))})) {
            const RunRow run = map_run_row(row);
            out.push_back(agora::core::run_branch_selection::RunRef{run.id, run.parent_run_id});
        }
        return out;
    }

private:
    static SqlValue opt(const std::optional<std::string>& value) {
        return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
    }

    static SqlValue opt(const std::optional<std::int64_t>& value) {
        return value.has_value() ? SqlValue::integer_value(*value) : SqlValue::null();
    }

    static std::optional<int> narrow(const SqlValue& cell) {
        const std::optional<std::int64_t> value = cell.integer_or_null();
        if (!value.has_value()) return std::nullopt;
        return static_cast<int>(*value);
    }

    static recovery::RecoveryMessage map_message_row(const SqlRow& row) {
        // MessageEntity's 26 columns in schema order; the converter applies to `images`.
        kotlin_check(row.size() == 26, "MessageEntity: row shape mismatch");
        recovery::RecoveryMessage message;
        message.id = row[0].as_text();
        // row[1] conversationId, row[2] parentId — never read by the recovery body.
        message.text = row[3].as_text();
        message.images = decode_string_list(row[4].as_text());
        message.thoughts = row[5].text_or_null();
        message.thought_title = row[6].text_or_null();
        message.token_count = static_cast<int>(row[7].as_integer());
        message.input_token_count = narrow(row[8]);
        message.cached_input_token_count = narrow(row[9]);
        message.cache_write_input_token_count = narrow(row[10]);
        message.uncached_input_token_count = narrow(row[11]);
        message.output_token_count = narrow(row[12]);
        message.reasoning_token_count = narrow(row[13]);
        message.generation_duration_ms = row[14].integer_or_null();
        message.status = message_status_from_wire(row[15].as_text());
        message.participant = participant_from_wire(row[16].as_text());
        // row[17] timestamp — not read by the body.
        message.thought_time_ms = row[18].integer_or_null();
        // row[19] modelName, row[21] attachmentMeta, row[22] runId, row[23] runSequence,
        // row[24] consumedAtPass, row[25] sourceJson — not read by the body.
        message.tool_call_json = row[20].text_or_null();
        return message;
    }

    SqliteExecutor& sql_;
    TransactionStack stack_;
};

// ---- driver --------------------------------------------------------------------------------

std::int64_t run_sql(SqliteExecutor& sql, const json::Value& step) {
    SqlArgs args;
    if (const json::Value* array = field(step, "args"); array != nullptr) {
        if (array->kind != json::Kind::Array) scenario_fail("args must be an array");
        for (const json::Value& argument : array->elements) {
            if (is_null_element(argument)) {
                args.push_back(SqlValue::null());
            } else if (argument.kind == json::Kind::String) {
                args.push_back(SqlValue::text_value(argument.text));
            } else {
                args.push_back(SqlValue::integer_value(std::stoll(argument.text)));
            }
        }
    }
    return sql.execute(required_string(step, "sql"), args);
}

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            sql.execute(
                "INSERT INTO conversations (id, selectedRunBranchesJson, dataChangedAt) "
                "VALUES (?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 optional_string(row, "selectedRunBranchesJson").has_value()
                     ? SqlValue::text_value(*optional_string(row, "selectedRunBranchesJson"))
                     : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "dataChangedAt").value_or(0))});
        }
    }
    if (const json::Value* runs = field(*seed_object, "runs"); runs != nullptr) {
        for (const json::Value& row : runs->elements) {
            const std::int64_t started_at = required_long(row, "startedAt");
            SqlArgs args;
            args.push_back(SqlValue::text_value(required_string(row, "id")));
            args.push_back(SqlValue::text_value(required_string(row, "conversationId")));
            const std::optional<std::string> parent = optional_string(row, "parentRunId");
            args.push_back(parent.has_value() ? SqlValue::text_value(*parent) : SqlValue::null());
            args.push_back(SqlValue::text_value(required_string(row, "status")));
            const std::optional<std::int64_t> slot = optional_long(row, "activeSlot");
            args.push_back(slot.has_value() ? SqlValue::integer_value(*slot) : SqlValue::null());
            args.push_back(SqlValue::integer_value(started_at));
            args.push_back(
                SqlValue::integer_value(optional_long(row, "lastCheckpointAt").value_or(started_at)));
            const std::optional<std::int64_t> stopped = optional_long(row, "stopRequestedAt");
            args.push_back(stopped.has_value() ? SqlValue::integer_value(*stopped) : SqlValue::null());
            const std::optional<std::int64_t> ended = optional_long(row, "endedAt");
            args.push_back(ended.has_value() ? SqlValue::integer_value(*ended) : SqlValue::null());
            const std::optional<std::string> reason = optional_string(row, "endReason");
            args.push_back(reason.has_value() ? SqlValue::text_value(*reason) : SqlValue::null());
            args.push_back(SqlValue::integer_value(optional_long(row, "currentPass").value_or(0)));
            const std::optional<std::string> ambiguous = optional_string(row, "legacyAmbiguous");
            args.push_back(
                SqlValue::integer_value(ambiguous.has_value() && *ambiguous == "true" ? 1 : 0));
            sql.execute(
                "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
                "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, "
                "legacyAmbiguous) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                args);
        }
    }
    if (const json::Value* messages = field(*seed_object, "messages"); messages != nullptr) {
        for (const json::Value& row : messages->elements) {
            // `images` seeds a JSON list (the converter's own encoding); `imagesRaw` seeds a
            // stored text verbatim, which is how the legacy "|||" format reaches the read path.
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
            args.push_back(thoughts.has_value() ? SqlValue::text_value(*thoughts) : SqlValue::null());
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
            args.push_back(SqlValue::text_value(required_string(row, "status")));
            args.push_back(SqlValue::text_value(required_string(row, "participant")));
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
                        {"selectedRunBranchesJson", optional_json_string(row[1].text_or_null())},
                        {"dataChangedAt", json_long(row[2].as_integer())}});
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

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"conversations",
          query_rows(sql,
                     "SELECT id, selectedRunBranchesJson, dataChangedAt FROM conversations "
                     "ORDER BY id",
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
                     message_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_RUNTIME_RECOVERY_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    SqliteRecoveryStore store(sql);

    std::vector<json::Value> steps;
    const json::Value* step_list = field(root, "steps");
    if (step_list != nullptr && step_list->kind == json::Kind::Array) {
        for (const json::Value& element : step_list->elements) {
            const std::string op = required_string(element, "op");
            json::Value result;
            try {
                if (op == "executeRuntimeRecovery") {
                    result = json_long(recovery::execute_runtime_recovery(
                        store, required_string(element, "conversationId"),
                        required_long(element, "at")));
                } else if (op == "sql") {
                    result = json_long(run_sql(sql, element));
                } else {
                    throw KotlinError("IllegalStateException", "unknown operation " + op);
                }
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

TEST(runtime_recovery_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_RUNTIME_RECOVERY_DIR;
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
