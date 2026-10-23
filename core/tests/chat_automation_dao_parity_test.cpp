// Golden parity test for data/local/ChatAutomationDao.kt.
//
// The stand runs the Kotlin interface against a real SQLite database through Room's replaced
// annotation processor; this test runs the C++ ChatAutomationDao (which owns the transcribed SQL,
// Room's @Upsert fallback and every CAS predicate) against the same schema (platform/schema.sql,
// shipped with the stand). All twenty members are driven — both observed queries as their initial
// emission — and every run ends with a dump of the task and loop tables.
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

#include "agora/core/chat_automation_dao.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace automation = agora::core::automation;
namespace json = agora::core::json;

using agora::core::KotlinError;
using agora::core::SqlArgs;
using agora::core::SqlError;
using agora::core::SqlRow;
using agora::core::SqlValue;
using automation::ChatAutomationDao;
using automation::LoopEntity;
using automation::TaskEntity;

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
        if ((rc & 0xFF) == SQLITE_CONSTRAINT) {
            throw agora::core::SqlConstraintError(message);
        }
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

// ---- helpers used by this module ----------------------------------------------------------

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

int required_int(const json::Value& object, std::string_view name) {
    return static_cast<int>(required_long(object, name));
}

json::Value json_bool(bool flag) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = flag ? "true" : "false";
    return value;
}

json::Value optional_json_int(const std::optional<int>& value) {
    if (!value.has_value()) return json_null();
    return json_long(*value);
}

// ---- seeding -------------------------------------------------------------------------------

void seed(SqliteExecutor& sql, const json::Value& root) {
    const json::Value* seed_object = field(root, "seed");
    if (seed_object == nullptr) return;
    if (const json::Value* conversations = field(*seed_object, "conversations");
        conversations != nullptr) {
        for (const json::Value& row : conversations->elements) {
            sql.execute(
                "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, "
                "selectedBranchesJson, systemPromptId, modelId, taskId, origin, graduated, "
                "draftText, draftAttachments, selectedRunBranchesJson, hasUnreadGeneration, "
                "isPinned) VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(optional_string(row, "title").value_or("")),
                 SqlValue::integer_value(optional_long(row, "lastUpdated").value_or(0)),
                 SqlValue::integer_value(optional_long(row, "dataChangedAt").value_or(0)),
                 SqlValue::null(), SqlValue::null(), SqlValue::null(), SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "origin").value_or("user")),
                 SqlValue::integer_value(0),
                 SqlValue::text_value(optional_string(row, "draftText").value_or("")),
                 SqlValue::null(), SqlValue::null(), SqlValue::integer_value(0),
                 SqlValue::integer_value(0)});
        }
    }
    if (const json::Value* tasks = field(*seed_object, "tasks"); tasks != nullptr) {
        for (const json::Value& row : tasks->elements) {
            const std::optional<std::string> system_prompt = optional_string(row, "systemPrompt");
            const std::optional<std::string> system_prompt_id =
                optional_string(row, "systemPromptId");
            const std::optional<std::string> model = optional_string(row, "modelId");
            const std::optional<std::int64_t> run_at = optional_long(row, "runAt");
            const std::optional<std::int64_t> last_run_at = optional_long(row, "lastRunAt");
            sql.execute(
                "INSERT INTO tasks (id, name, prompt, systemPrompt, systemPromptId, modelId, "
                "cronExpr, runAt, nextRunAt, enabled, createdAt, lastRunAt) "
                "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "id")),
                 SqlValue::text_value(optional_string(row, "name").value_or("")),
                 SqlValue::text_value(optional_string(row, "prompt").value_or("")),
                 system_prompt.has_value() ? SqlValue::text_value(*system_prompt)
                                           : SqlValue::null(),
                 system_prompt_id.has_value() ? SqlValue::text_value(*system_prompt_id)
                                              : SqlValue::null(),
                 model.has_value() ? SqlValue::text_value(*model) : SqlValue::null(),
                 SqlValue::text_value(optional_string(row, "cronExpr").value_or("")),
                 run_at.has_value() ? SqlValue::integer_value(*run_at) : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "nextRunAt").value_or(0)),
                 SqlValue::bool_value(optional_string(row, "enabled").value_or("true") != "false"),
                 SqlValue::integer_value(optional_long(row, "createdAt").value_or(0)),
                 last_run_at.has_value() ? SqlValue::integer_value(*last_run_at)
                                         : SqlValue::null()});
        }
    }
    if (const json::Value* loops = field(*seed_object, "loops"); loops != nullptr) {
        for (const json::Value& row : loops->elements) {
            const std::optional<std::string> prompt = optional_string(row, "prompt");
            const std::optional<std::int64_t> max_cycles = optional_long(row, "maxCycles");
            sql.execute(
                "INSERT INTO loops (conversationId, intervalMs, prompt, nextFireAt, cycleCount, "
                "maxCycles, active, revision) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
                {SqlValue::text_value(required_string(row, "conversationId")),
                 SqlValue::integer_value(optional_long(row, "intervalMs").value_or(0)),
                 prompt.has_value() ? SqlValue::text_value(*prompt) : SqlValue::null(),
                 SqlValue::integer_value(optional_long(row, "nextFireAt").value_or(0)),
                 SqlValue::integer_value(optional_long(row, "cycleCount").value_or(0)),
                 max_cycles.has_value() ? SqlValue::integer_value(*max_cycles)
                                        : SqlValue::null(),
                 SqlValue::bool_value(optional_string(row, "active").value_or("true") != "false"),
                 SqlValue::integer_value(optional_long(row, "revision").value_or(0))});
        }
    }
}

// ---- rendering ----------------------------------------------------------------------------

json::Value task_json(const TaskEntity& entity) {
    return json_object(
        {{"id", json_string(entity.id)},
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
         {"lastRunAt", optional_json_long(entity.last_run_at)}});
}

json::Value loop_json(const LoopEntity& entity) {
    return json_object(
        {{"conversationId", json_string(entity.conversation_id)},
         {"intervalMs", json_long(entity.interval_ms)},
         {"prompt", optional_json_string(entity.prompt)},
         {"nextFireAt", json_long(entity.next_fire_at)},
         {"cycleCount", json_long(entity.cycle_count)},
         {"maxCycles", optional_json_int(entity.max_cycles)},
         {"active", json_bool(entity.active)},
         {"revision", json_long(entity.revision)}});
}

// ---- driver --------------------------------------------------------------------------------

TaskEntity task_from_step(const json::Value& step) {
    TaskEntity entity;
    entity.id = required_string(step, "id");
    entity.name = optional_string(step, "name").value_or("");
    entity.prompt = optional_string(step, "prompt").value_or("");
    entity.system_prompt = optional_string(step, "systemPrompt");
    entity.system_prompt_id = optional_string(step, "systemPromptId");
    entity.model_id = optional_string(step, "modelId");
    entity.cron_expr = optional_string(step, "cronExpr").value_or("");
    entity.run_at = optional_long(step, "runAt");
    entity.next_run_at = optional_long(step, "nextRunAt").value_or(0);
    entity.enabled = optional_string(step, "enabled").value_or("true") != "false";
    entity.created_at = optional_long(step, "createdAt").value_or(0);
    entity.last_run_at = optional_long(step, "lastRunAt");
    return entity;
}

LoopEntity loop_from_step(const json::Value& step) {
    LoopEntity entity;
    entity.conversation_id = required_string(step, "conversationId");
    entity.interval_ms = optional_long(step, "intervalMs").value_or(0);
    entity.prompt = optional_string(step, "prompt");
    entity.next_fire_at = optional_long(step, "nextFireAt").value_or(0);
    const std::optional<std::int64_t> cycle = optional_long(step, "cycleCount");
    entity.cycle_count = cycle.has_value() ? static_cast<int>(*cycle) : 0;
    const std::optional<std::int64_t> max_cycles = optional_long(step, "maxCycles");
    entity.max_cycles =
        max_cycles.has_value() ? std::optional<int>(static_cast<int>(*max_cycles)) : std::nullopt;
    entity.active = optional_string(step, "active").value_or("true") != "false";
    entity.revision = optional_long(step, "revision").value_or(0);
    return entity;
}

json::Value dispatch(const std::string& op, const json::Value& step, ChatAutomationDao& dao) {
    if (op == "getAllTasks") {
        std::vector<json::Value> out;
        for (const TaskEntity& row : dao.get_all_tasks()) out.push_back(task_json(row));
        return json_array(std::move(out));
    }
    if (op == "getTask") {
        const std::optional<TaskEntity> row = dao.get_task(required_string(step, "id"));
        if (!row.has_value()) return json_null();
        return task_json(*row);
    }
    if (op == "getEnabledTasks") {
        std::vector<json::Value> out;
        for (const TaskEntity& row : dao.get_enabled_tasks()) out.push_back(task_json(row));
        return json_array(std::move(out));
    }
    if (op == "upsertTask") {
        dao.upsert_task(task_from_step(step));
        return json_null();
    }
    if (op == "replaceTaskModelReferences") {
        return json_long(dao.replace_task_model_references(
            required_string(step, "oldModelId"), optional_string(step, "newModelId")));
    }
    if (op == "renameTaskProviderModelReferences") {
        return json_long(dao.rename_task_provider_model_references(
            required_string(step, "oldProvider"), required_string(step, "newProvider")));
    }
    if (op == "updateTaskNextRunAtIfUnchanged") {
        return json_long(dao.update_task_next_run_at_if_unchanged(
            required_string(step, "id"), required_string(step, "expectedCronExpr"),
            required_long(step, "expectedNextRunAt"),
            required_long(step, "replacementNextRunAt")));
    }
    if (op == "deleteTask") {
        dao.delete_task(required_string(step, "id"));
        return json_null();
    }
    if (op == "deleteAllTasks") {
        dao.delete_all_tasks();
        return json_null();
    }
    if (op == "getAllTasksList") {
        std::vector<json::Value> out;
        for (const TaskEntity& row : dao.get_all_tasks_list()) out.push_back(task_json(row));
        return json_array(std::move(out));
    }
    if (op == "getLoop") {
        const std::optional<LoopEntity> row = dao.get_loop(required_string(step, "conversationId"));
        if (!row.has_value()) return json_null();
        return loop_json(*row);
    }
    if (op == "getActiveLoops") {
        std::vector<json::Value> out;
        for (const LoopEntity& row : dao.get_active_loops()) out.push_back(loop_json(row));
        return json_array(std::move(out));
    }
    if (op == "observeActiveLoops") {
        std::vector<json::Value> out;
        for (const LoopEntity& row : dao.observe_active_loops()) out.push_back(loop_json(row));
        return json_array(std::move(out));
    }
    if (op == "upsertLoopRow") {
        dao.upsert_loop_row(loop_from_step(step));
        return json_null();
    }
    if (op == "updateLoopNextFireAtIfUnchanged") {
        return json_long(dao.update_loop_next_fire_at_if_unchanged(
            required_string(step, "conversationId"), required_long(step, "expectedRevision"),
            required_int(step, "expectedCycleCount"), required_long(step, "expectedIntervalMs"),
            required_long(step, "expectedNextFireAt"),
            required_long(step, "replacementNextFireAt")));
    }
    if (op == "deactivateLoopRowIfUnchanged") {
        return json_long(dao.deactivate_loop_row_if_unchanged(
            required_string(step, "conversationId"), required_long(step, "expectedRevision"),
            required_int(step, "expectedCycleCount"), required_long(step, "expectedIntervalMs"),
            required_long(step, "expectedNextFireAt"),
            required_int(step, "normalizedMaxCycles")));
    }
    if (op == "deleteLoopRow") {
        return json_long(dao.delete_loop_row(required_string(step, "conversationId")));
    }
    if (op == "deleteAllLoops") {
        dao.delete_all_loops();
        return json_null();
    }
    if (op == "getAllLoopsList") {
        std::vector<json::Value> out;
        for (const LoopEntity& row : dao.get_all_loops_list()) out.push_back(loop_json(row));
        return json_array(std::move(out));
    }
    if (op == "getLoopsForConversationSnapshot") {
        std::vector<json::Value> out;
        for (const LoopEntity& row :
             dao.get_loops_for_conversation_snapshot(required_string(step, "conversationId"))) {
            out.push_back(loop_json(row));
        }
        return json_array(std::move(out));
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

json::Value task_row_json(const SqlRow& row) {
    return json_object(
        {{"id", json_string(row[0].as_text())},
         {"name", json_string(row[1].as_text())},
         {"prompt", json_string(row[2].as_text())},
         {"systemPrompt", optional_json_string(row[3].text_or_null())},
         {"systemPromptId", optional_json_string(row[4].text_or_null())},
         {"modelId", optional_json_string(row[5].text_or_null())},
         {"cronExpr", json_string(row[6].as_text())},
         {"runAt", optional_json_long(row[7].integer_or_null())},
         {"nextRunAt", json_long(row[8].as_integer())},
         {"enabled", json_bool(row[9].as_integer() != 0)},
         {"createdAt", json_long(row[10].as_integer())},
         {"lastRunAt", optional_json_long(row[11].integer_or_null())}});
}

json::Value loop_row_json(const SqlRow& row) {
    const std::optional<std::int64_t> max_cycles = row[5].integer_or_null();
    return json_object(
        {{"conversationId", json_string(row[0].as_text())},
         {"intervalMs", json_long(row[1].as_integer())},
         {"prompt", optional_json_string(row[2].text_or_null())},
         {"nextFireAt", json_long(row[3].as_integer())},
         {"cycleCount", json_long(row[4].as_integer())},
         {"maxCycles", max_cycles.has_value()
                           ? json_long(*max_cycles)
                           : json_null()},
         {"active", json_bool(row[6].as_integer() != 0)},
         {"revision", json_long(row[7].as_integer())}});
}

json::Value dump_tables(SqliteExecutor& sql) {
    return json_object(
        {{"tasks",
          query_rows(sql,
                     "SELECT id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, "
                     "runAt, nextRunAt, enabled, createdAt, lastRunAt FROM tasks ORDER BY id",
                     task_row_json)},
         {"loops",
          query_rows(sql,
                     "SELECT conversationId, intervalMs, prompt, nextFireAt, cycleCount, "
                     "maxCycles, active, revision FROM loops ORDER BY conversationId",
                     loop_row_json)}});
}

std::string run_scenario(const std::filesystem::path& scenario) {
    const std::filesystem::path root_dir = AGORA_FIXTURE_CHAT_AUTOMATION_DAO_DIR;
    const json::ParseResult parsed = json::parse(read_file(scenario));
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) {
        scenario_fail(scenario.stem().string() + ": scenario must be an object");
    }
    const json::Value& root = *parsed.value;

    SqliteExecutor sql(root_dir / "platform" / "schema.sql");
    seed(sql, root);
    ChatAutomationDao dao(sql);

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

TEST(chat_automation_dao_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CHAT_AUTOMATION_DAO_DIR;
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
