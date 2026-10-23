// Port of data/local/ChatAutomationDao.kt.
//
// Every SELECT uses the entity's declared column order (Room's `SELECT *` cursor order); the two
// upserts implement Room's INSERT-then-UPDATE-by-primary-key; the model-reference rewrites and
// clock CAS updates transcribe the annotation SQL verbatim. Named placeholders are bound in their
// order of appearance, which is the generated adapter's order.
#include "agora/core/chat_automation_dao.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::automation {

namespace {

constexpr const char* kTaskColumns =
    "id, name, prompt, systemPrompt, systemPromptId, modelId, cronExpr, runAt, nextRunAt, "
    "enabled, createdAt, lastRunAt";
constexpr const char* kLoopColumns =
    "conversationId, intervalMs, prompt, nextFireAt, cycleCount, maxCycles, active, revision";

SqlValue optional_text(const std::optional<std::string>& value) {
    return value.has_value() ? SqlValue::text_value(*value) : SqlValue::null();
}

TaskEntity read_task(const SqlRow& row) {
    kotlin_check(row.size() == 12, "TaskEntity: row shape mismatch");
    TaskEntity task;
    task.id = row[0].as_text();
    task.name = row[1].as_text();
    task.prompt = row[2].as_text();
    task.system_prompt = row[3].text_or_null();
    task.system_prompt_id = row[4].text_or_null();
    task.model_id = row[5].text_or_null();
    task.cron_expr = row[6].as_text();
    task.run_at = row[7].integer_or_null();
    task.next_run_at = row[8].as_integer();
    task.enabled = row[9].as_integer() != 0;
    task.created_at = row[10].as_integer();
    task.last_run_at = row[11].integer_or_null();
    return task;
}

LoopEntity read_loop(const SqlRow& row) {
    kotlin_check(row.size() == 8, "LoopEntity: row shape mismatch");
    LoopEntity loop;
    loop.conversation_id = row[0].as_text();
    loop.interval_ms = row[1].as_integer();
    loop.prompt = row[2].text_or_null();
    loop.next_fire_at = row[3].as_integer();
    loop.cycle_count = static_cast<int>(row[4].as_integer());
    const std::optional<std::int64_t> max_cycles = row[5].integer_or_null();
    loop.max_cycles =
        max_cycles.has_value() ? std::optional<int>(static_cast<int>(*max_cycles)) : std::nullopt;
    loop.active = row[6].as_integer() != 0;
    loop.revision = row[7].as_integer();
    return loop;
}

std::vector<TaskEntity> read_tasks(const std::vector<SqlRow>& rows) {
    std::vector<TaskEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(read_task(row));
    return out;
}

std::vector<LoopEntity> read_loops(const std::vector<SqlRow>& rows) {
    std::vector<LoopEntity> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) out.push_back(read_loop(row));
    return out;
}

std::vector<SqlRow> query_tasks(SqlExecutor& sql, const std::string& suffix,
                                std::vector<SqlValue> args = {}) {
    return sql.query(std::string("SELECT ") + kTaskColumns + " FROM tasks " + suffix,
                     std::move(args));
}

std::vector<SqlRow> query_loops(SqlExecutor& sql, const std::string& suffix,
                                std::vector<SqlValue> args = {}) {
    return sql.query(std::string("SELECT ") + kLoopColumns + " FROM loops " + suffix,
                     std::move(args));
}

}  // namespace

std::vector<TaskEntity> ChatAutomationDao::get_all_tasks() {
    return read_tasks(query_tasks(sql_, "ORDER BY createdAt DESC"));
}

std::optional<TaskEntity> ChatAutomationDao::get_task(std::string_view id) {
    const std::vector<SqlRow> rows =
        query_tasks(sql_, "WHERE id = ?", {SqlValue::text_value(std::string(id))});
    if (rows.empty()) return std::nullopt;
    return read_task(rows.front());
}

std::vector<TaskEntity> ChatAutomationDao::get_enabled_tasks() {
    return read_tasks(query_tasks(sql_, "WHERE enabled = 1"));
}

void ChatAutomationDao::upsert_task(const TaskEntity& task) {
    try {
        sql_.execute(
            "INSERT INTO tasks (id, name, prompt, systemPrompt, systemPromptId, modelId, "
            "cronExpr, runAt, nextRunAt, enabled, createdAt, lastRunAt) "
            "VALUES (?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?, ?)",
            {SqlValue::text_value(task.id), SqlValue::text_value(task.name),
             SqlValue::text_value(task.prompt), optional_text(task.system_prompt),
             optional_text(task.system_prompt_id), optional_text(task.model_id),
             SqlValue::text_value(task.cron_expr),
             task.run_at.has_value() ? SqlValue::integer_value(*task.run_at) : SqlValue::null(),
             SqlValue::integer_value(task.next_run_at), SqlValue::bool_value(task.enabled),
             SqlValue::integer_value(task.created_at),
             task.last_run_at.has_value() ? SqlValue::integer_value(*task.last_run_at)
                                          : SqlValue::null()});
    } catch (const SqlConstraintError&) {
        // Room's @Upsert: on a primary-key conflict, update every non-key column.
        sql_.execute(
            "UPDATE tasks SET name = ?, prompt = ?, systemPrompt = ?, systemPromptId = ?, "
            "modelId = ?, cronExpr = ?, runAt = ?, nextRunAt = ?, enabled = ?, createdAt = ?, "
            "lastRunAt = ? WHERE id = ?",
            {SqlValue::text_value(task.name), SqlValue::text_value(task.prompt),
             optional_text(task.system_prompt), optional_text(task.system_prompt_id),
             optional_text(task.model_id), SqlValue::text_value(task.cron_expr),
             task.run_at.has_value() ? SqlValue::integer_value(*task.run_at) : SqlValue::null(),
             SqlValue::integer_value(task.next_run_at), SqlValue::bool_value(task.enabled),
             SqlValue::integer_value(task.created_at),
             task.last_run_at.has_value() ? SqlValue::integer_value(*task.last_run_at)
                                          : SqlValue::null(),
             SqlValue::text_value(task.id)});
    }
}

int ChatAutomationDao::replace_task_model_references(
    std::string_view old_model_id, const std::optional<std::string>& new_model_id) {
    return static_cast<int>(sql_.execute(
        "UPDATE tasks SET modelId = ? WHERE modelId = ?",
        {optional_text(new_model_id), SqlValue::text_value(std::string(old_model_id))}));
}

int ChatAutomationDao::rename_task_provider_model_references(std::string_view old_provider,
                                                             std::string_view new_provider) {
    return static_cast<int>(sql_.execute(
        "UPDATE tasks SET modelId = ? || substr(modelId, length(?) + 1) "
        "WHERE substr(modelId, 1, length(?) + 1) = ? || ':'",
        {SqlValue::text_value(std::string(new_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider)),
         SqlValue::text_value(std::string(old_provider))}));
}

int ChatAutomationDao::update_task_next_run_at_if_unchanged(std::string_view id,
                                                            std::string_view expected_cron_expr,
                                                            std::int64_t expected_next_run_at,
                                                            std::int64_t replacement_next_run_at) {
    return static_cast<int>(sql_.execute(
        "UPDATE tasks SET nextRunAt = ? WHERE id = ? AND enabled = 1 AND cronExpr = ? "
        "AND nextRunAt = ?",
        {SqlValue::integer_value(replacement_next_run_at), SqlValue::text_value(std::string(id)),
         SqlValue::text_value(std::string(expected_cron_expr)),
         SqlValue::integer_value(expected_next_run_at)}));
}

void ChatAutomationDao::delete_task(std::string_view id) {
    sql_.execute("DELETE FROM tasks WHERE id = ?", {SqlValue::text_value(std::string(id))});
}

void ChatAutomationDao::delete_all_tasks() { sql_.execute("DELETE FROM tasks", {}); }

std::vector<TaskEntity> ChatAutomationDao::get_all_tasks_list() {
    return read_tasks(query_tasks(sql_, ""));
}

std::optional<LoopEntity> ChatAutomationDao::get_loop(std::string_view conversation_id) {
    const std::vector<SqlRow> rows =
        query_loops(sql_, "WHERE conversationId = ?",
                    {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    return read_loop(rows.front());
}

std::vector<LoopEntity> ChatAutomationDao::get_active_loops() {
    return read_loops(query_loops(sql_, "WHERE active = 1"));
}

std::vector<LoopEntity> ChatAutomationDao::observe_active_loops() {
    return read_loops(query_loops(sql_, "WHERE active = 1"));
}

void ChatAutomationDao::upsert_loop_row(const LoopEntity& loop) {
    try {
        sql_.execute(
            "INSERT INTO loops (conversationId, intervalMs, prompt, nextFireAt, cycleCount, "
            "maxCycles, active, revision) VALUES (?, ?, ?, ?, ?, ?, ?, ?)",
            {SqlValue::text_value(loop.conversation_id), SqlValue::integer_value(loop.interval_ms),
             optional_text(loop.prompt), SqlValue::integer_value(loop.next_fire_at),
             SqlValue::integer_value(loop.cycle_count),
             loop.max_cycles.has_value() ? SqlValue::integer_value(*loop.max_cycles)
                                         : SqlValue::null(),
             SqlValue::bool_value(loop.active), SqlValue::integer_value(loop.revision)});
    } catch (const SqlConstraintError&) {
        // Room's @Upsert: on a primary-key conflict, update every non-key column.
        sql_.execute(
            "UPDATE loops SET intervalMs = ?, prompt = ?, nextFireAt = ?, cycleCount = ?, "
            "maxCycles = ?, active = ?, revision = ? WHERE conversationId = ?",
            {SqlValue::integer_value(loop.interval_ms), optional_text(loop.prompt),
             SqlValue::integer_value(loop.next_fire_at), SqlValue::integer_value(loop.cycle_count),
             loop.max_cycles.has_value() ? SqlValue::integer_value(*loop.max_cycles)
                                         : SqlValue::null(),
             SqlValue::bool_value(loop.active), SqlValue::integer_value(loop.revision),
             SqlValue::text_value(loop.conversation_id)});
    }
}

int ChatAutomationDao::update_loop_next_fire_at_if_unchanged(
    std::string_view conversation_id,
    std::int64_t expected_revision,
    int expected_cycle_count,
    std::int64_t expected_interval_ms,
    std::int64_t expected_next_fire_at,
    std::int64_t replacement_next_fire_at) {
    return static_cast<int>(sql_.execute(
        "UPDATE loops SET nextFireAt = ? WHERE conversationId = ? AND active = 1 "
        "AND revision = ? AND cycleCount = ? AND intervalMs = ? AND nextFireAt = ?",
        {SqlValue::integer_value(replacement_next_fire_at),
         SqlValue::text_value(std::string(conversation_id)),
         SqlValue::integer_value(expected_revision), SqlValue::integer_value(expected_cycle_count),
         SqlValue::integer_value(expected_interval_ms),
         SqlValue::integer_value(expected_next_fire_at)}));
}

int ChatAutomationDao::deactivate_loop_row_if_unchanged(std::string_view conversation_id,
                                                        std::int64_t expected_revision,
                                                        int expected_cycle_count,
                                                        std::int64_t expected_interval_ms,
                                                        std::int64_t expected_next_fire_at,
                                                        int normalized_max_cycles) {
    return static_cast<int>(sql_.execute(
        "UPDATE loops SET active = 0, nextFireAt = 0, revision = revision + 1, "
        "maxCycles = ? WHERE conversationId = ? AND active = 1 AND revision = ? "
        "AND cycleCount = ? AND intervalMs = ? AND nextFireAt = ?",
        {SqlValue::integer_value(normalized_max_cycles),
         SqlValue::text_value(std::string(conversation_id)),
         SqlValue::integer_value(expected_revision), SqlValue::integer_value(expected_cycle_count),
         SqlValue::integer_value(expected_interval_ms),
         SqlValue::integer_value(expected_next_fire_at)}));
}

int ChatAutomationDao::delete_loop_row(std::string_view conversation_id) {
    return static_cast<int>(sql_.execute(
        "DELETE FROM loops WHERE conversationId = ?",
        {SqlValue::text_value(std::string(conversation_id))}));
}

void ChatAutomationDao::delete_all_loops() { sql_.execute("DELETE FROM loops", {}); }

std::vector<LoopEntity> ChatAutomationDao::get_all_loops_list() {
    return read_loops(query_loops(sql_, ""));
}

std::vector<LoopEntity> ChatAutomationDao::get_loops_for_conversation_snapshot(
    std::string_view conversation_id) {
    return read_loops(query_loops(sql_, "WHERE conversationId = ?",
                                  {SqlValue::text_value(std::string(conversation_id))}));
}

}  // namespace agora::core::automation
