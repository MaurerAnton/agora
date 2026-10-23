#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/sql_boundary.hpp"

namespace agora::core::automation {

/** TaskEntity (data/local/ChatEntities.kt): a saved prompt + schedule. */
struct TaskEntity {
    std::string id;
    std::string name;
    std::string prompt;
    std::optional<std::string> system_prompt;
    std::optional<std::string> system_prompt_id;
    std::optional<std::string> model_id;
    std::string cron_expr;
    std::optional<std::int64_t> run_at;
    std::int64_t next_run_at = 0;
    bool enabled = true;
    std::int64_t created_at = 0;
    std::optional<std::int64_t> last_run_at;
};

/** LoopEntity (data/local/ChatEntities.kt): a periodic in-context turn for one conversation. */
struct LoopEntity {
    std::string conversation_id;
    std::int64_t interval_ms = 0;
    std::optional<std::string> prompt;
    std::int64_t next_fire_at = 0;
    int cycle_count = 0;
    std::optional<int> max_cycles;
    bool active = true;
    std::int64_t revision = 0;
};

/**
 * Port of data/local/ChatAutomationDao.kt: task and loop persistence inherited by ChatDao.
 *
 * The two observed queries (`getAllTasks`, `getLoop`, `observeActiveLoops`) are Room Flows. The
 * C++ application has no coroutine stream, so the port exposes each query's current rows — the
 * Flow's initial emission; invalidation-driven re-emission is store behavior.
 *
 * @Upsert follows Room's generated algorithm: INSERT, and on a primary-key conflict UPDATE every
 * non-key column of the row with the same key. The CAS updates transcribe the annotation SQL
 * exactly, including the parameter order of the named placeholders.
 */
class ChatAutomationDao {
public:
    explicit ChatAutomationDao(SqlExecutor& sql) : sql_(sql) {}

    std::vector<TaskEntity> get_all_tasks();

    std::optional<TaskEntity> get_task(std::string_view id);

    std::vector<TaskEntity> get_enabled_tasks();

    void upsert_task(const TaskEntity& task);

    int replace_task_model_references(std::string_view old_model_id,
                                      const std::optional<std::string>& new_model_id);

    int rename_task_provider_model_references(std::string_view old_provider,
                                              std::string_view new_provider);

    int update_task_next_run_at_if_unchanged(std::string_view id,
                                             std::string_view expected_cron_expr,
                                             std::int64_t expected_next_run_at,
                                             std::int64_t replacement_next_run_at);

    void delete_task(std::string_view id);

    void delete_all_tasks();

    std::vector<TaskEntity> get_all_tasks_list();

    std::optional<LoopEntity> get_loop(std::string_view conversation_id);

    std::vector<LoopEntity> get_active_loops();

    std::vector<LoopEntity> observe_active_loops();

    void upsert_loop_row(const LoopEntity& loop);

    int update_loop_next_fire_at_if_unchanged(std::string_view conversation_id,
                                              std::int64_t expected_revision,
                                              int expected_cycle_count,
                                              std::int64_t expected_interval_ms,
                                              std::int64_t expected_next_fire_at,
                                              std::int64_t replacement_next_fire_at);

    int deactivate_loop_row_if_unchanged(std::string_view conversation_id,
                                         std::int64_t expected_revision,
                                         int expected_cycle_count,
                                         std::int64_t expected_interval_ms,
                                         std::int64_t expected_next_fire_at,
                                         int normalized_max_cycles);

    int delete_loop_row(std::string_view conversation_id);

    void delete_all_loops();

    std::vector<LoopEntity> get_all_loops_list();

    std::vector<LoopEntity> get_loops_for_conversation_snapshot(std::string_view conversation_id);

private:
    SqlExecutor& sql_;
};

}  // namespace agora::core::automation
