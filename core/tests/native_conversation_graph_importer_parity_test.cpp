// Golden parity tests for data/NativeConversationGraphImporter.kt.
//
// The Kotlin stand (core/tools/golden/native-conversation-graph-importer) compiles the importer
// and everything it touches verbatim (see its README) and freezes one trace per scenario: the
// counts, every ConversationGraphHeaders field, and the import's transfer id / schedule flag /
// table dump — with generated UUIDs masked as <uuid:N> and 13-digit epoch stamps as <clock>.
// This test replays each scenario through the C++ port over the same schema and requires the
// trace to match at the JSON value level.
//
// Transaction note: the stand executor reference-counts begin/commit/rollback so a nested DAO
// transaction joins the outer import transaction — the same single SQL transaction the JVM's
// SQLiteSession produces for Room's shared RoomDatabase stack (and what the Kotlin traces pin:
// every import lands atomically or not at all).

#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
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
#include <vector>

#include <sqlite3.h>
#include <unistd.h>

#include "agora/core/android_json.hpp"
#include "agora/core/chat_dao.hpp"
#include "agora/core/json.hpp"
#include "agora/core/maintenance_debt.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/native_conversation_graph_importer.hpp"
#include "agora/core/native_conversation_graph_source.hpp"
#include "agora/core/semantic_index_ledger.hpp"
#include "agora/core/sql_boundary.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace graph_import = agora::core::native_graph_import;
namespace native_graph = agora::core::native_graph;
using agora::core::KotlinError;
using agora::core::SqlArgs;
using agora::core::SqlExecutor;
using agora::core::SqlRow;
using agora::core::SqlValue;
namespace android_json = agora::core::android_json;
using json::Kind;
using json::Member;
using json::Value;

// ---- scenario/trace helpers --------------------------------------------------------------------

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

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

int required_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Literal) {
        scenario_fail(std::string(name) + " must be an int");
    }
    return std::stoi(value->text);
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

json::Value json_null() {
    Value v;
    v.kind = Kind::Literal;
    v.text = "null";
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

std::string excerpt(const json::Value& value, std::size_t limit = 500) {
    std::string text = json::to_kotlin_string(value);
    if (text.size() > limit) return text.substr(0, limit) + "...";
    return text;
}

// ---- the stand executor (depth-counted transactions; see the file header) -----------------------

class SqliteExecutor final : public SqlExecutor {
public:
    SqliteExecutor(const std::filesystem::path& schema_file, const Value& scenario) {
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
    }

    ~SqliteExecutor() override {
        if (db_ != nullptr) sqlite3_close(db_);
    }

    std::int64_t execute(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        bind_all(statement, args);
        const int rc = sqlite3_step(statement);
        if (rc != SQLITE_DONE) {
            const std::string message = sqlite3_errmsg(db_);
            sqlite3_finalize(statement);
            if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
                // The @Upsert ports catch SqlConstraintError to run their UPDATE branch.
                throw agora::core::SqlConstraintError(message);
            }
            throw agora::core::SqlError("sqlite: " + message);
        }
        const std::int64_t changed = sqlite3_changes(db_);
        sqlite3_finalize(statement);
        return changed;
    }

    std::vector<SqlRow> query(const std::string& sql, const SqlArgs& args) override {
        sqlite3_stmt* statement = prepare(sql);
        bind_all(statement, args);
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
            throw agora::core::SqlError("sqlite: " + message);
        }
        sqlite3_finalize(statement);
        return rows;
    }

    std::int64_t last_insert_rowid() override { return sqlite3_last_insert_rowid(db_); }

    // Reference-counted: nested DAO transactions join the outer import transaction, and only the
    // outermost level issues COMMIT/ROLLBACK — the JVM SQLiteSession behavior Room relies on.
    void begin() override {
        if (depth_++ == 0) {
            aborted_ = false;
            exec_raw("BEGIN");
        }
    }

    void commit() override {
        if (depth_ == 0) throw agora::core::SqlError("commit without a transaction");
        if (--depth_ == 0) {
            if (deferred_rollback_) {
                deferred_rollback_ = false;
                exec_raw("ROLLBACK");
            } else {
                exec_raw("COMMIT");
            }
        }
    }

    void rollback() override {
        if (depth_ == 0) throw agora::core::SqlError("rollback without a transaction");
        deferred_rollback_ = true;
        if (--depth_ == 0) {
            deferred_rollback_ = false;
            exec_raw("ROLLBACK");
        }
    }

    std::int64_t exec_count(const std::string& sql) {
        sqlite3_stmt* statement = prepare(sql);
        std::int64_t count = 0;
        if (sqlite3_step(statement) == SQLITE_ROW) count = sqlite3_column_int64(statement, 0);
        sqlite3_finalize(statement);
        return count;
    }

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
        if (sqlite3_prepare_v2(db_, sql.c_str(), -1, &statement, nullptr) != SQLITE_OK) {
            throw agora::core::SqlError(std::string("sqlite prepare: ") + sqlite3_errmsg(db_));
        }
        return statement;
    }

    void bind_all(sqlite3_stmt* statement, const SqlArgs& args) {
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
                case SqlValue::Kind::Blob:
                    sqlite3_bind_blob(statement, index, value.blob.data(),
                                      static_cast<int>(value.blob.size()), SQLITE_TRANSIENT);
                    break;
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
    int depth_ = 0;
    bool deferred_rollback_ = false;
    bool aborted_ = false;
};

// ---- the entry-source double --------------------------------------------------------------------

class MemoryEntrySource final : public native_graph::EntrySource {
public:
    std::map<std::string, std::string> entries;

    bool has(std::string_view name) override {
        return entries.find(std::string(name)) != entries.end();
    }

    std::optional<std::string> bytes(std::string_view name) override {
        const auto it = entries.find(std::string(name));
        if (it == entries.end()) return std::nullopt;
        return it->second;
    }

    std::unique_ptr<android_json::Source> stream(std::string_view name) override {
        const auto it = entries.find(std::string(name));
        if (it == entries.end()) return nullptr;
        return std::unique_ptr<android_json::Source>(new MemoryStreamSource(it->second));
    }

private:
    class MemoryStreamSource final : public android_json::Source {
    public:
        explicit MemoryStreamSource(const std::string& bytes) : bytes_(bytes) {}

        long long read(char* buffer, long long capacity) override {
            if (capacity <= 0) return 0;
            if (position_ >= bytes_.size()) return -1;
            const std::size_t take =
                std::min<std::size_t>(bytes_.size() - position_,
                                      static_cast<std::size_t>(capacity));
            std::memcpy(buffer, bytes_.data() + position_, take);
            position_ += take;
            return static_cast<long long>(take);
        }

    private:
        std::string bytes_;
        std::size_t position_ = 0;
    };
};

}  // namespace

namespace {

// ---- entity dumps (mirror the stand driver's builders) ------------------------------------------

json::Value json_bool(bool value) {
    Value v;
    v.kind = Kind::Literal;
    v.text = value ? "true" : "false";
    return v;
}

json::Value text_or_null(const std::optional<std::string>& value) {
    return value.has_value() ? json_string(*value) : json_null();
}

json::Value long_or_null(const std::optional<std::int64_t>& value) {
    return value.has_value() ? json_long(*value) : json_null();
}

json::Value int_or_null(const std::optional<int>& value) {
    return value.has_value() ? json_long(*value) : json_null();
}

json::Value conversation_json(const agora::core::ChatEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"title", json_string(entity.title)},
        {"lastUpdated", json_long(entity.last_updated)},
        {"dataChangedAt", json_long(entity.data_changed_at)},
        {"selectedBranchesJson", text_or_null(entity.selected_branches_json)},
        {"systemPromptId", text_or_null(entity.system_prompt_id)},
        {"modelId", text_or_null(entity.model_id)},
        {"taskId", text_or_null(entity.task_id)},
        {"origin", json_string(entity.origin)},
        {"graduated", json_bool(entity.graduated)},
        {"draftText", json_string(entity.draft_text)},
        {"draftAttachments", text_or_null(entity.draft_attachments)},
        {"selectedRunBranchesJson", text_or_null(entity.selected_run_branches_json)},
        {"hasUnreadGeneration", json_bool(entity.has_unread_generation)},
        {"isPinned", json_bool(entity.is_pinned)},
    });
}

json::Value run_json(const agora::core::context_compact::RunEntity& entity) {
    std::string status;
    switch (entity.status) {
        case agora::core::conversation_runtime::RunStatus::Active: status = "ACTIVE"; break;
        case agora::core::conversation_runtime::RunStatus::Stopping: status = "STOPPING"; break;
        case agora::core::conversation_runtime::RunStatus::Completed: status = "COMPLETED"; break;
        case agora::core::conversation_runtime::RunStatus::Stopped: status = "STOPPED"; break;
        default: status = "FAILED"; break;
    }
    std::optional<std::string> end_reason;
    if (entity.end_reason.has_value()) {
        switch (*entity.end_reason) {
            case agora::core::conversation_runtime::RunEndReason::ModelCompleted:
                end_reason = "MODEL_COMPLETED"; break;
            case agora::core::conversation_runtime::RunEndReason::UserStopped:
                end_reason = "USER_STOPPED"; break;
            case agora::core::conversation_runtime::RunEndReason::ProcessRecovered:
                end_reason = "PROCESS_RECOVERED"; break;
            default: end_reason = "FAILED"; break;
        }
    }
    return json_object({
        {"id", json_string(entity.id)},
        {"conversationId", json_string(entity.conversation_id)},
        {"parentRunId", text_or_null(entity.parent_run_id)},
        {"status", json_string(status)},
        {"activeSlot", int_or_null(entity.active_slot)},
        {"startedAt", json_long(entity.started_at)},
        {"lastCheckpointAt", json_long(entity.last_checkpoint_at)},
        {"stopRequestedAt", long_or_null(entity.stop_requested_at)},
        {"endedAt", long_or_null(entity.ended_at)},
        {"endReason", text_or_null(end_reason)},
        {"currentPass", json_long(entity.current_pass)},
        {"legacyAmbiguous", json_bool(entity.legacy_ambiguous)},
    });
}

json::Value task_json(const agora::core::automation::TaskEntity& entity) {
    return json_object({
        {"id", json_string(entity.id)},
        {"name", json_string(entity.name)},
        {"prompt", json_string(entity.prompt)},
        {"systemPrompt", text_or_null(entity.system_prompt)},
        {"systemPromptId", text_or_null(entity.system_prompt_id)},
        {"modelId", text_or_null(entity.model_id)},
        {"cronExpr", json_string(entity.cron_expr)},
        {"runAt", long_or_null(entity.run_at)},
        {"nextRunAt", json_long(entity.next_run_at)},
        {"enabled", json_bool(entity.enabled)},
        {"createdAt", json_long(entity.created_at)},
        {"lastRunAt", long_or_null(entity.last_run_at)},
    });
}

json::Value loop_json(const agora::core::automation::LoopEntity& entity) {
    return json_object({
        {"conversationId", json_string(entity.conversation_id)},
        {"intervalMs", json_long(entity.interval_ms)},
        {"prompt", text_or_null(entity.prompt)},
        {"nextFireAt", json_long(entity.next_fire_at)},
        {"cycleCount", json_long(entity.cycle_count)},
        {"maxCycles", int_or_null(entity.max_cycles)},
        {"active", json_bool(entity.active)},
        {"revision", json_long(entity.revision)},
    });
}

json::Value conversation_settings_json(
    const agora::core::export_extra_settings::ConversationSettings& settings) {
    std::vector<std::pair<std::string, json::Value>> members;
    auto put_int = [&](std::string_view key, const std::optional<int>& value) {
        if (value.has_value()) members.emplace_back(key, json_long(*value));
    };
    auto put_float = [&](std::string_view key, const std::optional<float>& value) {
        if (value.has_value()) {
            members.emplace_back(
                key, json_string(agora::core::openai::format_kotlin_float(*value)));
        }
    };
    auto put_bool = [&](std::string_view key, const std::optional<bool>& value) {
        if (value.has_value()) members.emplace_back(key, json_bool(*value));
    };
    auto put_text = [&](std::string_view key, const std::optional<std::string>& value) {
        if (value.has_value()) members.emplace_back(key, json_string(*value));
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
    return json_object(std::move(members));
}

json::Value headers_json(const graph_import::ConversationGraphHeaders& headers) {
    std::vector<json::Value> tasks;
    for (const auto& task : headers.tasks) tasks.push_back(task_json(task));
    std::vector<json::Value> conversations;
    for (const auto& conversation : headers.conversations) {
        conversations.push_back(conversation_json(conversation));
    }
    std::vector<json::Value> runs;
    for (const auto& run : headers.runs) runs.push_back(run_json(run));
    std::vector<json::Value> loops;
    for (const auto& loop : headers.loops) loops.push_back(loop_json(loop));
    std::vector<json::Value> available;
    for (const auto& id : headers.available_conversation_ids) available.push_back(json_string(id));
    std::vector<std::pair<std::string, json::Value>> settings_members;
    for (const auto& [id, settings] : headers.conversation_settings) {
        settings_members.emplace_back(id, conversation_settings_json(settings));
    }
    return json_object({
        {"tasks", json_array(std::move(tasks))},
        {"conversations", json_array(std::move(conversations))},
        {"runs", json_array(std::move(runs))},
        {"sourceRunIdsWereUnique", json_bool(headers.source_run_ids_were_unique)},
        {"loops", json_array(std::move(loops))},
        {"availableConversationIds", json_array(std::move(available))},
        {"conversationSettings", json_object(std::move(settings_members))},
    });
}

// ---- table dump (the stand driver's exact queries and renders) -----------------------------------

json::Value cell_text(const SqlRow& row, std::size_t index) {
    const std::optional<std::string> value = row[index].text_or_null();
    return value.has_value() ? json_string(*value) : json_null();
}

json::Value cell_long(const SqlRow& row, std::size_t index) {
    const std::optional<std::int64_t> value = row[index].integer_or_null();
    return value.has_value() ? json_long(*value) : json_null();
}

/** Kotlin getObject(i) as? Number — null stays null, anything numeric truncates to an int. */
json::Value cell_optional_int(const SqlRow& row, std::size_t index) {
    if (row[index].is_null()) return json_null();
    if (row[index].kind == SqlValue::Kind::Integer) return json_long(row[index].integer);
    if (row[index].kind == SqlValue::Kind::Real) {
        return json_long(static_cast<std::int64_t>(row[index].real));
    }
    return json_null();
}

json::Value cell_optional_long(const SqlRow& row, std::size_t index) {
    if (row[index].is_null()) return json_null();
    if (row[index].kind == SqlValue::Kind::Real) {
        return json_long(static_cast<std::int64_t>(row[index].real));
    }
    return cell_long(row, index);
}

/** Kotlin getInt(i) != 0 — a NULL reads as 0, so null is false. */
json::Value cell_bool(const SqlRow& row, std::size_t index) {
    if (row[index].is_null()) return json_bool(false);
    if (row[index].kind == SqlValue::Kind::Real) {
        return json_bool(static_cast<std::int64_t>(row[index].real) != 0);
    }
    return json_bool(row[index].integer != 0);
}

json::Value query_rows(SqlExecutor& sql, const std::string& statement,
                       const std::function<json::Value(const SqlRow&)>& render) {
    std::vector<json::Value> rows;
    for (const SqlRow& row : sql.query(statement, {})) rows.push_back(render(row));
    return json_array(std::move(rows));
}

json::Value dump_tables(SqlExecutor& sql) {
    return json_object({
        {"conversations", query_rows(
            sql,
            "SELECT id, title, lastUpdated, dataChangedAt, selectedBranchesJson, systemPromptId, "
            "modelId, taskId, origin, graduated, draftText, draftAttachments, "
            "selectedRunBranchesJson, hasUnreadGeneration, isPinned FROM conversations ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_text(row, 0)},
                    {"title", cell_text(row, 1)},
                    {"lastUpdated", cell_long(row, 2)},
                    {"dataChangedAt", cell_long(row, 3)},
                    {"selectedBranchesJson", cell_text(row, 4)},
                    {"systemPromptId", cell_text(row, 5)},
                    {"modelId", cell_text(row, 6)},
                    {"taskId", cell_text(row, 7)},
                    {"origin", cell_text(row, 8)},
                    {"graduated", cell_bool(row, 9)},
                    {"draftText", cell_text(row, 10)},
                    {"draftAttachments", cell_text(row, 11)},
                    {"selectedRunBranchesJson", cell_text(row, 12)},
                    {"hasUnreadGeneration", cell_bool(row, 13)},
                    {"isPinned", cell_bool(row, 14)},
                });
            })},
        {"runs", query_rows(
            sql,
            "SELECT id, conversationId, parentRunId, status, activeSlot, startedAt, "
            "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous "
            "FROM runs ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_text(row, 0)},
                    {"conversationId", cell_text(row, 1)},
                    {"parentRunId", cell_text(row, 2)},
                    {"status", cell_text(row, 3)},
                    {"activeSlot", cell_optional_int(row, 4)},
                    {"startedAt", cell_long(row, 5)},
                    {"lastCheckpointAt", cell_long(row, 6)},
                    {"stopRequestedAt", cell_optional_long(row, 7)},
                    {"endedAt", cell_optional_long(row, 8)},
                    {"endReason", cell_text(row, 9)},
                    {"currentPass", cell_long(row, 10)},
                    {"legacyAmbiguous", cell_bool(row, 11)},
                });
            })},
        {"messages", query_rows(
            sql,
            "SELECT id, conversationId, parentId, text, images, tokenCount, status, participant, "
            "timestamp, modelName, attachmentMeta, runId, runSequence, consumedAtPass, "
            "sourceJson, toolCallJson FROM messages ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_text(row, 0)},
                    {"conversationId", cell_text(row, 1)},
                    {"parentId", cell_text(row, 2)},
                    {"text", cell_text(row, 3)},
                    {"images", cell_text(row, 4)},
                    {"tokenCount", cell_long(row, 5)},
                    {"status", cell_text(row, 6)},
                    {"participant", cell_text(row, 7)},
                    {"timestamp", cell_long(row, 8)},
                    {"modelName", cell_text(row, 9)},
                    {"attachmentMeta", cell_text(row, 10)},
                    {"runId", cell_text(row, 11)},
                    {"runSequence", cell_long(row, 12)},
                    {"consumedAtPass", cell_optional_int(row, 13)},
                    {"sourceJson", cell_text(row, 14)},
                    {"toolCallJson", cell_text(row, 15)},
                });
            })},
        {"tasks", query_rows(
            sql,
            "SELECT id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, runAt, "
            "nextRunAt, enabled, createdAt, lastRunAt FROM tasks ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_text(row, 0)},
                    {"name", cell_text(row, 1)},
                    {"prompt", cell_text(row, 2)},
                    {"systemPrompt", cell_text(row, 3)},
                    {"systemPromptId", cell_text(row, 4)},
                    {"modelId", cell_text(row, 5)},
                    {"cronExpr", cell_text(row, 6)},
                    {"runAt", cell_optional_long(row, 7)},
                    {"nextRunAt", cell_long(row, 8)},
                    {"enabled", cell_bool(row, 9)},
                    {"createdAt", cell_long(row, 10)},
                    {"lastRunAt", cell_optional_long(row, 11)},
                });
            })},
        {"loops", query_rows(
            sql,
            "SELECT conversationId, intervalMs, prompt, nextFireAt, cycleCount, maxCycles, active, "
            "revision FROM loops ORDER BY conversationId",
            [](const SqlRow& row) {
                return json_object({
                    {"conversationId", cell_text(row, 0)},
                    {"intervalMs", cell_long(row, 1)},
                    {"prompt", cell_text(row, 2)},
                    {"nextFireAt", cell_long(row, 3)},
                    {"cycleCount", cell_long(row, 4)},
                    {"maxCycles", cell_optional_int(row, 5)},
                    {"active", cell_bool(row, 6)},
                    {"revision", cell_long(row, 7)},
                });
            })},
        {"embeddings", query_rows(
            sql,
            "SELECT id, messageId, modelId, chunkText, dimension FROM embeddings ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_long(row, 0)},
                    {"messageId", cell_text(row, 1)},
                    {"modelId", cell_text(row, 2)},
                    {"chunkText", cell_text(row, 3)},
                    {"dimension", cell_long(row, 4)},
                });
            })},
        {"conversation_settings_import_transfer", query_rows(
            sql,
            "SELECT id, transferId, settingsJson, mode FROM conversation_settings_import_transfer "
            "ORDER BY id",
            [](const SqlRow& row) {
                return json_object({
                    {"id", cell_long(row, 0)},
                    {"transferId", cell_text(row, 1)},
                    {"settingsJson", cell_text(row, 2)},
                    {"mode", cell_text(row, 3)},
                });
            })},
        {"maintenance_debt", query_rows(
            sql,
            "SELECT kind, identity, state, revision, updatedAt FROM maintenance_debt "
            "ORDER BY kind, identity",
            [](const SqlRow& row) {
                return json_object({
                    {"kind", cell_text(row, 0)},
                    {"identity", cell_text(row, 1)},
                    {"state", cell_text(row, 2)},
                    {"revision", cell_long(row, 3)},
                    {"updatedAt", cell_long(row, 4)},
                });
            })},
        {"semantic_index_ledger", query_rows(
            sql,
            "SELECT modelId, state, sourceRevision, completedRevision, reconcileRevision, updatedAt "
            "FROM semantic_index_ledger ORDER BY modelId",
            [](const SqlRow& row) {
                return json_object({
                    {"modelId", cell_text(row, 0)},
                    {"state", cell_text(row, 1)},
                    {"sourceRevision", cell_long(row, 2)},
                    {"completedRevision", cell_long(row, 3)},
                    {"reconcileRevision", cell_long(row, 4)},
                    {"updatedAt", cell_long(row, 5)},
                });
            })},
    });
}

// ---- masking (identical to the stand driver's two passes) -----------------------------------------

std::string mask_trace(std::string text) {
    // UUIDs: first-appearance ordinal replacement. Fixture content never carries UUID-shaped
    // values; the generated ones are the settings-transfer id and planner-made run ids.
    static const std::regex uuid_pattern(
        "[0-9a-fA-F]{8}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{4}-[0-9a-fA-F]{12}");
    std::map<std::string, std::string> ordinals;
    std::string masked;
    auto begin = std::sregex_iterator(text.begin(), text.end(), uuid_pattern);
    std::size_t copied = 0;
    for (auto it = begin; it != std::sregex_iterator(); ++it) {
        const std::smatch& match = *it;
        const std::string uuid = match.str();
        const auto [entry, inserted] =
            ordinals.emplace(uuid, "<uuid:" + std::to_string(ordinals.size()) + ">");
        (void)inserted;
        masked.append(text, copied,
                      static_cast<std::size_t>(match.position(0)) - copied);
        copied = static_cast<std::size_t>(match.position(0) + match.length(0));
        masked += entry->second;
    }
    masked.append(text, copied, std::string::npos);

    // Epochs: any 13-digit 1[7-9] value bounded by non-digits (the driver's lookarounds) ->
    // a JSON string "<clock>". Hand-scanned so the boundary rules are exactly the Kotlin ones.
    std::string out;
    out.reserve(masked.size());
    for (std::size_t i = 0; i < masked.size();) {
        const auto digit = [](char c) { return c >= '0' && c <= '9'; };
        if (digit(masked[i]) && i + 13 <= masked.size()) {
            bool candidate = (masked[i + 1] == '7' || masked[i + 1] == '8' ||
                              masked[i + 1] == '9');
            for (std::size_t k = 0; k < 13 && candidate; ++k) candidate = digit(masked[i + k]);
            if (candidate) {
                const bool before_ok = i == 0 || (!digit(masked[i - 1]) && masked[i - 1] != '.');
                const bool after_ok = i + 13 == masked.size() || !digit(masked[i + 13]);
                if (before_ok && after_ok) {
                    out += "\"<clock>\"";
                    i += 13;
                    continue;
                }
            }
        }
        out.push_back(masked[i]);
        ++i;
    }
    return out;
}

// ---- scenario runner ------------------------------------------------------------------------------

graph_import::RestoredMedia build_restored_media(const Value& scenario) {
    graph_import::RestoredMedia restored;
    const Value* element = field(scenario, "restoredMedia");
    if (element == nullptr || element->kind != Kind::Object) return restored;
    if (const Value* files = field(*element, "archiveFiles");
        files != nullptr && files->kind == Kind::Object) {
        for (const Member& member : files->members) {
            if (member.value.kind != Kind::Object) {
                scenario_fail("restoredMedia.archiveFiles entries must be objects");
            }
            restored.archive_files[member.key] = graph_import::RestoredMediaFile{
                required_string(member.value, "absolutePath"),
                required_string(member.value, "uri"),
            };
        }
    }
    auto legacy_index = [&](std::string_view name,
                            std::map<std::string, std::map<int, std::string>>& out_map) {
        const Value* map_value = field(*element, name);
        if (map_value == nullptr || map_value->kind != Kind::Object) return;
        for (const Member& message : map_value->members) {
            if (message.value.kind != Kind::Object) {
                scenario_fail(std::string(name) + " entries must be objects");
            }
            std::map<int, std::string>& inner = out_map[message.key];
            for (const Member& index : message.value.members) {
                if (index.value.kind != Kind::String) {
                    scenario_fail(std::string(name) + " values must be strings");
                }
                inner[std::stoi(index.key)] = index.value.text;
            }
        }
    };
    legacy_index("legacyImagesByMessage", restored.legacy_images_by_message);
    legacy_index("legacyVideosByMessage", restored.legacy_videos_by_message);
    return restored;
}

void run_scenario(const std::filesystem::path& scenario_path,
                  const std::filesystem::path& golden_path,
                  const std::filesystem::path& schema_path) {
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail("scenario is not an object: " + scenario_path.filename().string());
    }
    const Value& scenario = *parsed.value;

    static int scenario_counter = 0;
    const std::filesystem::path cache_dir =
        std::filesystem::temp_directory_path() /
        ("agora-graph-import-cpp-" + std::to_string(scenario_counter++) + "-" +
         std::to_string(::getpid()));
    std::error_code ignored;
    std::filesystem::remove_all(cache_dir, ignored);
    std::filesystem::create_directories(cache_dir);
    struct Cleanup {
        std::filesystem::path path;
        ~Cleanup() {
            std::error_code ec;
            std::filesystem::remove_all(path, ec);
        }
    } cleanup{cache_dir};

    SqliteExecutor sql(schema_path, scenario);
    ::agora::core::chat_dao::ChatDao dao(sql);
    ::agora::core::semantic_index::SemanticIndexDao semantic(sql);
    ::agora::core::maintenance_debt::MaintenanceDebtDao debt(sql);

    // The injected resolveSystemPromptId: containsKey semantics over the scenario map, nulls pass
    // through (matches the stand driver's lambda).
    std::map<std::string, std::optional<std::string>> resolve_map;
    if (const Value* mapping = field(scenario, "resolveSystemPromptIds");
        mapping != nullptr && mapping->kind == Kind::Object) {
        for (const Member& member : mapping->members) {
            if (member.value.kind == Kind::Literal && member.value.text == "null") {
                resolve_map[member.key] = std::nullopt;
            } else if (member.value.kind == Kind::String) {
                resolve_map[member.key] = member.value.text;
            } else {
                scenario_fail("resolveSystemPromptIds values must be strings or null");
            }
        }
    }

    int uuid_counter = 0;
    bool scheduled = false;
    graph_import::ImportCallbacks callbacks;
    callbacks.resolve_system_prompt_id =
        [&](const std::optional<std::string>& id) -> std::optional<std::string> {
            if (!id.has_value()) return std::nullopt;
            const auto it = resolve_map.find(*id);
            if (it != resolve_map.end()) return it->second;
            return id;
        };
    callbacks.new_uuid = [&uuid_counter] {
        char buffer[37];
        std::snprintf(buffer, sizeof(buffer), "%08x-0000-4000-8000-%012x", uuid_counter,
                      uuid_counter);
        ++uuid_counter;
        return std::string(buffer);
    };
    // A 13-digit epoch (the mask rewrites it); any wall clock would also do.
    callbacks.now_millis = [] { return static_cast<std::int64_t>(1790000000000LL); };
    callbacks.schedule_maintenance = [&scheduled] { scheduled = true; };

    graph_import::NativeConversationGraphImporter importer(dao, semantic, debt, sql, callbacks);

    MemoryEntrySource archive;
    if (const Value* entries = field(scenario, "entries");
        entries != nullptr && entries->kind == Kind::Object) {
        for (const Member& member : entries->members) {
            if (member.value.kind != Kind::String) scenario_fail("entries must be strings");
            archive.entries[member.key] = member.value.text;
        }
    }
    const int graph_version = [&] {
        const Value* value = field(scenario, "graphVersion");
        if (value == nullptr || value->kind != Kind::Literal) return 5;
        return std::stoi(value->text);
    }();
    const graph_import::RestoredMedia restored = build_restored_media(scenario);

    const Value* steps = field(scenario, "steps");
    if (steps == nullptr || steps->kind != Kind::Array) scenario_fail("scenario has no steps");

    std::vector<json::Value> trace_steps;
    for (const Value& step : steps->elements) {
        const std::string op = required_string(step, "op");
        std::string strategy_name = "MERGE";
        if (const Value* step_strategy = field(step, "strategy")) {
            strategy_name = step_strategy->text;
        } else if (const Value* scenario_strategy = field(scenario, "strategy")) {
            strategy_name = scenario_strategy->text;
        }
        const graph_import::ImportStrategy strategy =
            strategy_name == "REPLACE" ? graph_import::ImportStrategy::Replace
            : strategy_name == "MERGE"  ? graph_import::ImportStrategy::Merge
                                        : graph_import::ImportStrategy::Skip;

        json::Value result;
        std::optional<json::Value> tables;
        try {
            if (op == "count") {
                native_graph::NativeConversationGraphSource source =
                    native_graph::NativeConversationGraphSource::open(archive, graph_version,
                                                                      cache_dir);
                try {
                    native_graph::NativeConversationGraphSource::Stream stream =
                        source.open_stream();
                    graph_import::GraphStreamAdapter adapter(stream);
                    const graph_import::GraphCounts counts = importer.count(adapter);
                    result = json_object({
                        {"conversations", json_long(counts.conversations)},
                        {"tasks", json_long(counts.tasks)},
                        {"loops", json_long(counts.loops)},
                    });
                } catch (...) {
                    source.close();
                    throw;
                }
                source.close();
            } else if (op == "headers") {
                native_graph::NativeConversationGraphSource source =
                    native_graph::NativeConversationGraphSource::open(archive, graph_version,
                                                                      cache_dir);
                graph_import::ConversationGraphHeaders headers;
                try {
                    native_graph::NativeConversationGraphSource::Stream stream =
                        source.open_stream();
                    graph_import::GraphStreamAdapter adapter(stream);
                    headers = importer.read_headers(adapter, strategy, restored);
                } catch (...) {
                    source.close();
                    throw;
                }
                source.close();
                result = headers_json(headers);
            } else if (op == "import") {
                scheduled = false;
                const int archive_version = [&] {
                    const Value* value = field(step, "archiveVersion");
                    if (value == nullptr || value->kind != Kind::Literal) return 6;
                    return std::stoi(value->text);
                }();
                native_graph::NativeConversationGraphSource source =
                    native_graph::NativeConversationGraphSource::open(archive, graph_version,
                                                                      cache_dir);
                std::string transfer_id;
                try {
                    // The real call site reads the headers from the graph first.
                    native_graph::NativeConversationGraphSource::Stream stream =
                        source.open_stream();
                    graph_import::GraphStreamAdapter adapter(stream);
                    const graph_import::ConversationGraphHeaders headers =
                        importer.read_headers(adapter, strategy, restored);

                    ::agora::core::semantic_index::SemanticModelSnapshot snapshot;
                    if (const Value* element = field(scenario, "semanticSnapshot");
                        element != nullptr && element->kind == Kind::Array &&
                        element->elements.size() == 2) {
                        const Value& first = element->elements[0];
                        if (first.kind == Kind::String) snapshot.active_model_id = first.text;
                        for (const Value& id : element->elements[1].elements) {
                            if (id.kind != Kind::String) {
                                scenario_fail("semanticSnapshot ids must be strings");
                            }
                            snapshot.configured_model_ids.push_back(id.text);
                        }
                    }
                    transfer_id = importer.import_graph(source, strategy, headers, restored,
                                                        archive_version, snapshot);
                } catch (...) {
                    source.close();
                    throw;
                }
                source.close();

                result = json_object({
                    {"transferId", json_string(transfer_id)},
                    {"scheduled", json_bool(scheduled)},
                });
                tables = dump_tables(sql);
            } else {
                scenario_fail("unknown step op " + op);
            }
        } catch (const android_json::Exception& error) {
            result = json_object({{"error", json_object({
                                                {"type", json_string(error.java_class())},
                                                {"message", json_string(error.what())},
                                            })}});
        } catch (const agora::core::SqlError& error) {
            result = json_object({{"error", json_object({
                                                {"type", json_string("RuntimeException")},
                                                {"message", json_string(error.what())},
                                            })}});
        }

        std::vector<std::pair<std::string, json::Value>> members{
            {"op", json_string(op)},
            {"result", std::move(result)},
        };
        if (tables.has_value()) members.emplace_back("tables", std::move(*tables));
        trace_steps.push_back(json_object(std::move(members)));
    }

    const json::Value actual =
        json_object({{"steps", json_array(std::move(trace_steps))}});
    const json::ParseResult golden = json::parse(read_file(golden_path));
    if (!golden.value) scenario_fail("golden trace is not JSON: " + golden_path.string());
    const json::Value masked = json::parse(mask_trace(json::to_kotlin_string(actual))).value
                                   .value_or(json_null());
    if (!json_equal(masked, *golden.value)) {
        scenario_fail(scenario_path.filename().string() + "\n  expected: " +
                      excerpt(*golden.value) + "\n  actual:   " + excerpt(masked));
    }
}

void test_native_conversation_graph_importer_golden_parity() {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_CONVERSATION_GRAPH_IMPORTER_DIR;
    const std::filesystem::path scenario_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenario_dir)) {
        if (entry.is_regular_file() && entry.path().extension() == ".json") {
            scenarios.push_back(entry.path());
        }
    }
    std::sort(scenarios.begin(), scenarios.end());
    if (scenarios.empty()) scenario_fail("no graph-importer scenarios found");
    for (const std::filesystem::path& scenario : scenarios) {
        run_scenario(scenario, golden_dir / (scenario.stem().string() + ".trace"),
                     root / "platform" / "schema.sql");
    }
}

}  // namespace

TEST(native_conversation_graph_importer_golden_parity) {
    test_native_conversation_graph_importer_golden_parity();
}
