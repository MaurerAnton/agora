#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_tool_provider.hpp"

// tool/AutomationToolProvider.kt: model-facing controls for saved Tasks and the current
// conversation's Loop. The feature flag is checked while publishing definitions AND again at
// execution time (the second check protects against stale tool calls produced before the user
// disabled automation tools). Cron validation reuses the pinned core CronExpression and the
// Loop bounds re-use loop_policy, so the interpolated schema copy can never drift from policy.

namespace agora::core::automation {

/** data/local/TaskEntity as the provider reads it for TaskEntity.toJson(). */
struct TaskInfo {
    std::string id;
    std::string name;
    std::string prompt;
    std::optional<std::string> model;           // modelId → "model"
    std::optional<std::string> system_prompt_id;
    std::string cron;                           // cronExpr → "cron"
    bool enabled = true;
    std::int64_t created_at = 0;
    std::optional<std::int64_t> last_run_at;
    std::int64_t next_run_at = 0;
};

/** data/local/LoopEntity as the provider reads it (interval kept in ms; toJson divides). */
struct LoopInfo {
    std::string conversation_id;
    std::int64_t interval_ms = 0;
    std::optional<std::string> prompt;
    std::int64_t next_fire_at = 0;
    std::int32_t cycle_count = 0;
    std::optional<std::int32_t> max_cycles;
    bool active = false;
};

/** data/SystemPromptEntry — only the fields createTask's resolution reads. */
struct SavedSystemPrompt {
    std::string id;
    std::string title;
};

/** TaskManager seam (automation/TaskManager.kt). */
class TaskSeam {
public:
    virtual ~TaskSeam() = default;

    virtual TaskInfo create_task(const std::string& name, const std::string& prompt,
                                 const std::string& cron,
                                 const std::optional<std::string>& model,
                                 const std::optional<std::string>& system_prompt_id) = 0;

    virtual std::vector<TaskInfo> list_tasks_snapshot() = 0;

    struct DeleteOutcome {
        enum class Kind { Deleted, NotFound, Ambiguous };
        Kind kind = Kind::NotFound;
        TaskInfo task;                  // Deleted
        std::vector<TaskInfo> matches;  // Ambiguous
    };

    virtual DeleteOutcome delete_task_by_id_or_name(const std::string& id_or_name) = 0;
};

/** LoopManager seam (automation/LoopManager.kt). */
class LoopSeam {
public:
    virtual ~LoopSeam() = default;

    struct StartOutcome {
        enum class Kind { Started, Conflict, Invalid, ConversationMissing };
        Kind kind = Kind::Invalid;
        LoopInfo loop;       // Started
        LoopInfo existing;   // Conflict (existing.nextFireAt)
        std::string reason;  // Invalid
    };

    virtual StartOutcome start_loop(const std::string& conversation_id, std::int64_t interval_ms,
                                    const std::optional<std::string>& prompt,
                                    std::int32_t max_cycles) = 0;

    enum class StopKind { Stopped, AlreadyStopped, NotFound };

    virtual StopKind stop_loop(const std::string& conversation_id) = 0;
};

class AutomationToolProvider final : public shell_provider::ToolProvider {
public:
    /**
     * The two Kotlin suspend lambdas become plain std::functions (the core execute() is
     * synchronous; the production bridge suspends on the coroutine side).
     */
    explicit AutomationToolProvider(
        TaskSeam& task_manager, LoopSeam& loop_manager,
        std::function<std::vector<SavedSystemPrompt>()> saved_system_prompts = [] {
            return std::vector<SavedSystemPrompt>{};
        },
        std::function<bool()> is_currently_enabled = [] { return true; });

    std::vector<openai::ToolDefinition> definitions(
        const shell_provider::GenerationContext& ctx) const override;

    bool handles(const std::string& name) const override;

    std::string execute(const std::string& name, const std::string& arguments,
                        const shell_provider::GenerationContext& ctx) override;

private:
    static std::string error(const std::string& message);
    static json::Value task_json(const TaskInfo& task);
    static json::Value loop_json(const LoopInfo& loop);

    std::string create_task(const json::Value& args);
    std::string list_tasks();
    std::string delete_task(const json::Value& args);
    std::string start_loop(const json::Value& args, const shell_provider::GenerationContext& ctx);
    std::string stop_loop(const shell_provider::GenerationContext& ctx);

    TaskSeam& task_manager_;
    LoopSeam& loop_manager_;
    std::function<std::vector<SavedSystemPrompt>()> saved_system_prompts_;
    std::function<bool()> is_currently_enabled_;
};

}  // namespace agora::core::automation