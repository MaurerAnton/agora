// Unit tests for the AutomationToolProvider port: the five tool definitions (with their
// loop_policy interpolation), the double feature-flag gate, argument validation, the seam
// calls (TaskManager / LoopManager) and every result/error envelope.

#include "agora/core/automation_tool_provider.hpp"

#include <optional>
#include <string>
#include <vector>

#include "agora/core/loop_policy.hpp"
#include "check.hpp"

namespace automation = agora::core::automation;
namespace shell_provider = agora::core::shell_provider;

namespace {

namespace loop = agora::core::loop_policy;

struct ScriptedTasks final : automation::TaskSeam {
    int create_calls = 0;
    std::string created_name;
    std::string created_prompt;
    std::string created_cron;
    std::optional<std::string> created_model;
    std::optional<std::string> created_system_prompt;

    std::vector<automation::TaskInfo> snapshot;
    automation::TaskSeam::DeleteOutcome delete_outcome;
    std::string last_delete_arg;

    automation::TaskInfo create_task(const std::string& name, const std::string& prompt,
                                     const std::string& cron,
                                     const std::optional<std::string>& model,
                                     const std::optional<std::string>& system_prompt_id) override {
        ++create_calls;
        created_name = name;
        created_prompt = prompt;
        created_cron = cron;
        created_model = model;
        created_system_prompt = system_prompt_id;
        automation::TaskInfo task;
        task.id = "task-1";
        task.name = name;
        task.prompt = prompt;
        task.cron = cron;
        task.model = model;
        task.system_prompt_id = system_prompt_id;
        task.enabled = true;
        task.created_at = 1000;
        task.next_run_at = 2000;
        return task;
    }

    std::vector<automation::TaskInfo> list_tasks_snapshot() override { return snapshot; }

    automation::TaskSeam::DeleteOutcome delete_task_by_id_or_name(
        const std::string& id_or_name) override {
        last_delete_arg = id_or_name;
        return delete_outcome;
    }
};

struct ScriptedLoops final : automation::LoopSeam {
    struct StartCall {
        std::string conversation_id;
        std::int64_t interval_ms = 0;
        std::optional<std::string> prompt;
        std::int32_t max_cycles = 0;
    };

    StartOutcome start_outcome;
    std::vector<StartCall> starts;
    StopKind stop_result = StopKind::Stopped;
    std::vector<std::string> stops;

    ScriptedLoops() { start_outcome.kind = StartOutcome::Kind::Started; }

    StartOutcome start_loop(const std::string& conversation_id, std::int64_t interval_ms,
                            const std::optional<std::string>& prompt,
                            std::int32_t max_cycles) override {
        starts.push_back(StartCall{conversation_id, interval_ms, prompt, max_cycles});
        return start_outcome;
    }

    StopKind stop_loop(const std::string& conversation_id) override {
        stops.push_back(conversation_id);
        return stop_result;
    }
};

struct Fixture {
    ScriptedTasks tasks;
    ScriptedLoops loops;
    std::vector<automation::SavedSystemPrompt> prompts;
    bool enabled = true;
    automation::AutomationToolProvider provider;

    Fixture()
        : provider(
              tasks, loops,
              [this] { return prompts; },
              [this] { return enabled; }) {}

    automation::TaskInfo make_task(std::string id, std::string name) {
        automation::TaskInfo task;
        task.id = std::move(id);
        task.name = std::move(name);
        task.prompt = "p";
        task.cron = "0 0 * * *";
        task.created_at = 1;
        task.next_run_at = 2;
        return task;
    }
};

shell_provider::GenerationContext make_ctx() {
    shell_provider::GenerationContext ctx;
    ctx.automation_tools_enabled = true;
    ctx.conversation_id = "conv-1";
    return ctx;
}

}  // namespace

// ---- Definitions ----------------------------------------------------------------------------------

TEST(automation_definitions_gated_by_context) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.automation_tools_enabled = false;
    CHECK(fixture.provider.definitions(ctx).empty());
    // handles() is independent of the gate.
    CHECK(fixture.provider.handles("create_task"));
    CHECK(fixture.provider.handles("list_tasks"));
    CHECK(fixture.provider.handles("delete_task"));
    CHECK(fixture.provider.handles("start_loop"));
    CHECK(fixture.provider.handles("stop_loop"));
    CHECK(!fixture.provider.handles("web_search"));
}

TEST(automation_definitions_exact_shapes) {
    Fixture fixture;
    const auto definitions = fixture.provider.definitions(make_ctx());
    CHECK_EQ(std::size_t(5), definitions.size());

    CHECK_EQ(std::string("create_task"), definitions[0].function.name);
    CHECK_EQ(
        std::string("Create an enabled background task with a 5-field cron schedule. Use only "
                    "when the user explicitly asks to create a recurring task."),
        definitions[0].function.description);
    const auto& create_props = definitions[0].function.parameters.properties;
    CHECK_EQ(std::size_t(5), create_props.size());
    CHECK_EQ(std::string("name"), create_props[0].first);
    CHECK_EQ(std::string("A short, descriptive task name."), create_props[0].second.description);
    CHECK_EQ(std::string("prompt"), create_props[1].first);
    CHECK_EQ(std::string("The complete prompt to run on every occurrence."),
             create_props[1].second.description);
    CHECK_EQ(std::string("cron"), create_props[2].first);
    CHECK_EQ(std::string("A valid 5-field cron expression: minute hour day-of-month month "
                         "day-of-week."),
             create_props[2].second.description);
    CHECK_EQ(std::string("model"), create_props[3].first);
    CHECK_EQ(std::string("Optional provider-prefixed model id. Omit to use the app default model."),
             create_props[3].second.description);
    CHECK_EQ(std::string("system_prompt"), create_props[4].first);
    CHECK_EQ(std::string("Optional saved system prompt, given as its id or its exact title. "
                         "Omit to use the app default prompt."),
             create_props[4].second.description);
    CHECK_EQ(std::size_t(3), definitions[0].function.parameters.required.size());
    CHECK_EQ(std::string("name"), definitions[0].function.parameters.required[0]);
    CHECK_EQ(std::string("prompt"), definitions[0].function.parameters.required[1]);
    CHECK_EQ(std::string("cron"), definitions[0].function.parameters.required[2]);

    CHECK_EQ(std::string("list_tasks"), definitions[1].function.name);
    CHECK_EQ(
        std::string("List all saved background tasks, including ids, schedules, enabled state, "
                    "and next run times."),
        definitions[1].function.description);
    CHECK(definitions[1].function.parameters.properties.empty());

    CHECK_EQ(std::string("delete_task"), definitions[2].function.name);
    CHECK_EQ(
        std::string("Delete one saved task by exact id or unique task name. This is destructive; "
                    "use only when the user explicitly asks."),
        definitions[2].function.description);
    CHECK_EQ(std::size_t(1), definitions[2].function.parameters.properties.size());
    CHECK_EQ(std::string("id_or_name"),
             definitions[2].function.parameters.properties[0].first);
    CHECK_EQ(std::string("The exact task id or a unique task name."),
             definitions[2].function.parameters.properties[0].second.description);
    CHECK_EQ(std::size_t(1), definitions[2].function.parameters.required.size());

    // The Loop bounds interpolate from loop_policy itself, so schema copy tracks the policy.
    CHECK_EQ(std::string("start_loop"), definitions[3].function.name);
    CHECK_EQ(std::string("Start a Loop in the current conversation. interval_seconds must be " +
                         std::to_string(loop::kMinIntervalSeconds) + " to " +
                         std::to_string(loop::kMaxIntervalSeconds) + ". max_cycles defaults to " +
                         std::to_string(loop::kDefaultMaxCycles) + " and must be " +
                         std::to_string(loop::kMinMaxCycles) + " to " +
                         std::to_string(loop::kMaxMaxCycles) + "."),
             definitions[3].function.description);
    const auto& loop_props = definitions[3].function.parameters.properties;
    CHECK_EQ(std::size_t(3), loop_props.size());
    CHECK_EQ(std::string("interval_seconds"), loop_props[0].first);
    CHECK_EQ(std::string("Seconds between cycles, from " +
                          std::to_string(loop::kMinIntervalSeconds) + " through " +
                          std::to_string(loop::kMaxIntervalSeconds) + " (7 days)."),
             loop_props[0].second.description);
    CHECK_EQ(std::string("prompt"), loop_props[1].first);
    CHECK_EQ(std::string("Optional prompt injected each cycle. Blank or omitted means Continue."),
             loop_props[1].second.description);
    CHECK_EQ(std::string("max_cycles"), loop_props[2].first);
    CHECK_EQ(std::string("Optional safety limit from " + std::to_string(loop::kMinMaxCycles) +
                          " through " + std::to_string(loop::kMaxMaxCycles) + ". Defaults to " +
                          std::to_string(loop::kDefaultMaxCycles) + "."),
             loop_props[2].second.description);
    CHECK_EQ(std::size_t(1), definitions[3].function.parameters.required.size());
    CHECK_EQ(std::string("interval_seconds"), definitions[3].function.parameters.required[0]);

    CHECK_EQ(std::string("stop_loop"), definitions[4].function.name);
    CHECK_EQ(std::string("Stop the Loop attached to the current conversation."),
             definitions[4].function.description);
    CHECK(definitions[4].function.parameters.properties.empty());
}

// ---- The double gate and dispatch ---------------------------------------------------------------------

TEST(automation_execute_gates_and_unknown_tool) {
    Fixture fixture;
    auto ctx = make_ctx();
    ctx.automation_tools_enabled = false;
    CHECK_EQ(std::string("Error: Automation tools are disabled"),
             fixture.provider.execute("list_tasks", "{}", ctx));

    ctx.automation_tools_enabled = true;
    fixture.enabled = false;  // the second, execution-time check
    CHECK_EQ(std::string("Error: Automation tools are disabled"),
             fixture.provider.execute("list_tasks", "{}", ctx));

    fixture.enabled = true;
    CHECK_EQ(std::string("Error: Unknown automation tool: nope"),
             fixture.provider.execute("nope", "{}", ctx));
}

TEST(automation_bad_arguments_envelope) {
    Fixture fixture;
    const auto ctx = make_ctx();
    // Malformed JSON → the kotlinx-style parse message behind the Error: wrapper.
    std::string result = fixture.provider.execute("list_tasks", "{oops", ctx);
    CHECK(result.rfind("Error: Unexpected JSON token at offset ", 0) == 0);
    // Non-object top level → "Element … is not a JsonObject".
    result = fixture.provider.execute("list_tasks", "[1]", ctx);
    CHECK_EQ(std::string("Error: Element [1] is not a JsonObject"), result);
    // Blank arguments become "{}" (Kotlin ifBlank) and fall into the tool's own validation.
    CHECK_EQ(std::string("Error: name is required"),
             fixture.provider.execute("create_task", "   ", ctx));
}

// ---- create_task ------------------------------------------------------------------------------------

TEST(automation_create_task_validation) {
    Fixture fixture;
    const auto ctx = make_ctx();
    CHECK_EQ(std::string("Error: name is required"),
             fixture.provider.execute("create_task", R"({"prompt":"p","cron":"0 * * * *"})", ctx));
    CHECK_EQ(std::string("Error: prompt is required"),
             fixture.provider.execute(
                 "create_task", R"({"name":"n","cron":"0 * * * *"})", ctx));
    CHECK_EQ(std::string("Error: cron is required"),
             fixture.provider.execute("create_task", R"({"name":"n","prompt":"p"})", ctx));
    // Non-string fields fall through string() → required-but-missing.
    CHECK_EQ(std::string("Error: name is required"),
             fixture.provider.execute(
                 "create_task", R"({"name":5,"prompt":"p","cron":"0 * * * *"})", ctx));
    // Cron validation (CronExpression.isValid, extracted verbatim into core).
    CHECK_EQ(std::string("Error: Invalid 5-field cron expression: not-a-cron"),
             fixture.provider.execute(
                 "create_task", R"({"name":"n","prompt":"p","cron":"not-a-cron"})", ctx));
    CHECK_EQ(0, fixture.tasks.create_calls);
}

TEST(automation_create_task_success_and_plumbing) {
    Fixture fixture;
    const auto ctx = make_ctx();
    const std::string result = fixture.provider.execute(
        "create_task",
        R"({"name":"  My Task  ","prompt":"  Do the thing  ","cron":" 0 * * * * ",)"
        R"("model":"  openai/gpt-5  ","system_prompt":"  "})",
        ctx);
    CHECK_EQ(1, fixture.tasks.create_calls);
    CHECK_EQ(std::string("My Task"), fixture.tasks.created_name);      // trimmed
    CHECK_EQ(std::string("Do the thing"), fixture.tasks.created_prompt);
    CHECK_EQ(std::string("0 * * * *"), fixture.tasks.created_cron);
    CHECK(fixture.tasks.created_model.has_value());
    CHECK_EQ(std::string("openai/gpt-5"), *fixture.tasks.created_model);
    CHECK(!fixture.tasks.created_system_prompt.has_value());  // whitespace-only → omitted

    CHECK_EQ(
        std::string(
            R"({"type":"create_task","task":{"id":"task-1","name":"My Task",)")
                .append(R"("prompt":"Do the thing","model":"openai/gpt-5",)")
                .append(R"("cron":"0 * * * *","enabled":true,"created_at":1000,)")
                .append(R"("next_run_at":2000}})"),
        result);
}

TEST(automation_create_task_system_prompt_resolution) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.prompts = {
        {"id-1", "Summary style"},
        {"id-2", "Terse Mode"},
        {"id-3", "terse mode"},  // same title, different case → ambiguous
    };

    // Exact id wins (Kotlin checks ids before titles).
    fixture.provider.execute("create_task",
                             R"({"name":"a","prompt":"b","cron":"0 * * * *","system_prompt":"id-1"})",
                             ctx);
    CHECK(fixture.tasks.created_system_prompt.has_value());
    CHECK_EQ(std::string("id-1"), *fixture.tasks.created_system_prompt);

    // Unique title match (trimmed + case-insensitive).
    fixture.provider.execute(
        "create_task",
        R"({"name":"a","prompt":"b","cron":"0 * * * *","system_prompt":"  summary STYLE  "})",
        ctx);
    CHECK(fixture.tasks.created_system_prompt.has_value());
    CHECK_EQ(std::string("id-1"), *fixture.tasks.created_system_prompt);

    // Ambiguous titles → fail closed, and the task is not created.
    fixture.tasks.create_calls = 0;
    CHECK_EQ(std::string("Error: Multiple saved system prompts are titled \"terse mode\"; pass "
                         "the prompt id instead"),
             fixture.provider.execute(
                 "create_task",
                 R"({"name":"a","prompt":"b","cron":"0 * * * *","system_prompt":"terse mode"})",
                 ctx));
    CHECK_EQ(0, fixture.tasks.create_calls);

    // Unknown title → not found.
    CHECK_EQ(std::string("Error: Saved system prompt not found: Mystery"),
             fixture.provider.execute(
                 "create_task",
                 R"({"name":"a","prompt":"b","cron":"0 * * * *","system_prompt":"Mystery"})",
                 ctx));
}

// ---- list / delete ---------------------------------------------------------------------------------

TEST(automation_list_tasks_envelope) {
    Fixture fixture;
    const auto ctx = make_ctx();
    automation::TaskInfo with_optionals;
    with_optionals.id = "t2";
    with_optionals.name = "B";
    with_optionals.prompt = "p2";
    with_optionals.model = "openai/gpt-5";
    with_optionals.system_prompt_id = "id-1";
    with_optionals.cron = "5 4 * * 0";
    with_optionals.enabled = false;
    with_optionals.created_at = 10;
    with_optionals.last_run_at = 20;
    with_optionals.next_run_at = 30;
    fixture.tasks.snapshot = {fixture.make_task("t1", "A"), with_optionals};

    CHECK_EQ(
        std::string(
            R"({"type":"list_tasks","tasks":[)")
                .append(R"({"id":"t1","name":"A","prompt":"p","cron":"0 0 * * *",)")
                .append(R"("enabled":true,"created_at":1,"next_run_at":2},)")
                .append(R"({"id":"t2","name":"B","prompt":"p2","model":"openai/gpt-5",)")
                .append(R"("system_prompt_id":"id-1","cron":"5 4 * * 0","enabled":false,)")
                .append(R"("created_at":10,"last_run_at":20,"next_run_at":30}]})"),
        fixture.provider.execute("list_tasks", "", ctx));
}

TEST(automation_delete_task_outcomes) {
    Fixture fixture;
    const auto ctx = make_ctx();

    CHECK_EQ(std::string("Error: id_or_name is required"),
             fixture.provider.execute("delete_task", R"({"id_or_name":"  "})", ctx));
    CHECK(fixture.tasks.last_delete_arg.empty());

    fixture.tasks.delete_outcome.kind = automation::TaskSeam::DeleteOutcome::Kind::NotFound;
    CHECK_EQ(std::string("Error: Task not found: ghost"),
             fixture.provider.execute("delete_task", R"({"id_or_name":"ghost"})", ctx));
    CHECK_EQ(std::string("ghost"), fixture.tasks.last_delete_arg);

    fixture.tasks.delete_outcome.kind = automation::TaskSeam::DeleteOutcome::Kind::Ambiguous;
    fixture.tasks.delete_outcome.matches = {fixture.make_task("id-1", "Alpha"),
                                             fixture.make_task("id-2", "Beta")};
    CHECK_EQ(std::string("Error: Multiple tasks match 'Al': Alpha (id-1), Beta (id-2)"),
             fixture.provider.execute("delete_task", R"({"id_or_name":"Al"})", ctx));

    fixture.tasks.delete_outcome.kind = automation::TaskSeam::DeleteOutcome::Kind::Deleted;
    fixture.tasks.delete_outcome.task = fixture.make_task("id-1", "Alpha");
    CHECK_EQ(std::string(
                 R"({"type":"delete_task","deleted":true,"task":)")
                 .append(R"({"id":"id-1","name":"Alpha","prompt":"p","cron":"0 0 * * *",)")
                 .append(R"("enabled":true,"created_at":1,"next_run_at":2}})"),
             fixture.provider.execute("delete_task", R"({"id_or_name":"id-1"})", ctx));
}

// ---- start_loop / stop_loop --------------------------------------------------------------------------

TEST(automation_start_loop_validation) {
    Fixture fixture;
    auto ctx = make_ctx();

    // Conversation required.
    ctx.conversation_id = std::nullopt;
    CHECK_EQ(std::string("Error: start_loop requires a current conversation"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));
    ctx.conversation_id = "   ";  // blank → missing
    CHECK_EQ(std::string("Error: start_loop requires a current conversation"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));

    ctx.conversation_id = "conv-1";
    // interval_seconds: missing, non-numeric and non-primitive all report the same error.
    CHECK_EQ(std::string("Error: interval_seconds must be an integer"),
             fixture.provider.execute("start_loop", "{}", ctx));
    CHECK_EQ(std::string("Error: interval_seconds must be an integer"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":"abc"})", ctx));
    CHECK_EQ(std::string("Error: interval_seconds must be an integer"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":{}})", ctx));
    // Out of the loop_policy range.
    CHECK_EQ(std::string("Error: interval_seconds must be between 10 and 604800"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":9})", ctx));
    CHECK_EQ(std::string("Error: interval_seconds must be between 10 and 604800"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":604801})", ctx));
    // Numeric strings parse (content-based, like kotlinx longOrNull).
    fixture.provider.execute("start_loop", R"({"interval_seconds":"60"})", ctx);
    CHECK_EQ(1, static_cast<int>(fixture.loops.starts.size()));
    CHECK_EQ(60'000, fixture.loops.starts.back().interval_ms);

    // max_cycles: present-but-invalid routes through the Int parse; absent uses the default.
    CHECK_EQ(std::string("Error: max_cycles must be an integer"),
             fixture.provider.execute(
                 "start_loop", R"({"interval_seconds":60,"max_cycles":"abc"})", ctx));
    CHECK_EQ(std::string("Error: max_cycles must be an integer"),
             fixture.provider.execute(
                 "start_loop", R"({"interval_seconds":60,"max_cycles":null})", ctx));
    CHECK_EQ(std::string("Error: max_cycles must be an integer"),
             fixture.provider.execute(
                 "start_loop", R"({"interval_seconds":60,"max_cycles":2147483648})", ctx));
    CHECK_EQ(std::string("Error: max_cycles must be between 1 and 100"),
             fixture.provider.execute(
                 "start_loop", R"({"interval_seconds":60,"max_cycles":0})", ctx));
    CHECK_EQ(std::string("Error: max_cycles must be between 1 and 100"),
             fixture.provider.execute(
                 "start_loop", R"({"interval_seconds":60,"max_cycles":101})", ctx));

    fixture.loops.starts.clear();
    fixture.provider.execute("start_loop",
                             R"({"interval_seconds":60,"max_cycles":25,"prompt":"  go  "})",
                             ctx);
    CHECK_EQ(1, static_cast<int>(fixture.loops.starts.size()));
    const auto& call = fixture.loops.starts.back();
    CHECK_EQ(std::string("conv-1"), call.conversation_id);
    CHECK_EQ(60'000, call.interval_ms);
    CHECK_EQ(25, call.max_cycles);
    CHECK(call.prompt.has_value());
    CHECK_EQ(std::string("go"), *call.prompt);
}

TEST(automation_start_loop_outcomes) {
    Fixture fixture;
    const auto ctx = make_ctx();
    fixture.loops.start_outcome.kind = automation::LoopSeam::StartOutcome::Kind::Started;
    fixture.loops.start_outcome.loop.conversation_id = "conv-1";
    fixture.loops.start_outcome.loop.interval_ms = 60'000;
    fixture.loops.start_outcome.loop.next_fire_at = 555;
    fixture.loops.start_outcome.loop.cycle_count = 0;
    fixture.loops.start_outcome.loop.active = true;

    CHECK_EQ(std::string(
                 R"({"type":"start_loop","status":"started","loop":)")
                 .append(R"({"conversation_id":"conv-1","interval_seconds":60,)")
                 .append(R"("next_fire_at":555,"cycle_count":0,"active":true}})"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));

    fixture.loops.start_outcome.kind = automation::LoopSeam::StartOutcome::Kind::Conflict;
    fixture.loops.start_outcome.existing.next_fire_at = 987654;
    CHECK_EQ(std::string("Error: A Loop is already active for this conversation "
                         "(next fire at 987654)"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));

    fixture.loops.start_outcome.kind = automation::LoopSeam::StartOutcome::Kind::Invalid;
    fixture.loops.start_outcome.reason = "interval must be between 10 and 604800 seconds";
    CHECK_EQ(std::string("Error: interval must be between 10 and 604800 seconds"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));

    fixture.loops.start_outcome.kind =
        automation::LoopSeam::StartOutcome::Kind::ConversationMissing;
    CHECK_EQ(std::string("Error: Conversation not found: conv-1"),
             fixture.provider.execute("start_loop", R"({"interval_seconds":60})", ctx));
}

TEST(automation_stop_loop_outcomes) {
    Fixture fixture;
    auto ctx = make_ctx();

    ctx.conversation_id = std::nullopt;
    CHECK_EQ(std::string("Error: stop_loop requires a current conversation"),
             fixture.provider.execute("stop_loop", "{}", ctx));

    ctx.conversation_id = "conv-1";
    fixture.loops.stop_result = automation::LoopSeam::StopKind::Stopped;
    CHECK_EQ(std::string(R"({"type":"stop_loop","status":"stopped","conversation_id":"conv-1"})"),
             fixture.provider.execute("stop_loop", "{}", ctx));
    fixture.loops.stop_result = automation::LoopSeam::StopKind::AlreadyStopped;
    CHECK_EQ(
        std::string(R"({"type":"stop_loop","status":"already_stopped","conversation_id":"conv-1"})"),
        fixture.provider.execute("stop_loop", "{}", ctx));
    fixture.loops.stop_result = automation::LoopSeam::StopKind::NotFound;
    CHECK_EQ(std::string("Error: No Loop exists for conversation: conv-1"),
             fixture.provider.execute("stop_loop", "{}", ctx));
    CHECK_EQ(std::size_t(3), fixture.loops.stops.size());
}