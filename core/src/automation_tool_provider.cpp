#include "agora/core/automation_tool_provider.hpp"

#include <algorithm>
#include <cctype>
#include <stdexcept>
#include <utility>

#include "agora/core/cron_expression.hpp"
#include "agora/core/loop_policy.hpp"

namespace agora::core::automation {

namespace {

namespace json = agora::core::json;
namespace loop = agora::core::loop_policy;

constexpr const char* kCreateTask = "create_task";
constexpr const char* kListTasks = "list_tasks";
constexpr const char* kDeleteTask = "delete_task";
constexpr const char* kStartLoop = "start_loop";
constexpr const char* kStopLoop = "stop_loop";

bool is_tool_name(const std::string& name) {
    return name == kCreateTask || name == kListTasks || name == kDeleteTask ||
           name == kStartLoop || name == kStopLoop;
}

// ---- JSON builders / Kotlin accessors ------------------------------------------------------------

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
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
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

std::string render(const json::Value& value) { return json::to_kotlin_string(value); }

const json::Value* map_get(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `(element as? JsonPrimitive)?.content` — JsonNull's content is the unquoted "null". */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::String || value->kind == json::Kind::Literal) return value->text;
    return std::nullopt;
}

/** `JsonPrimitive.longOrNull` / `intOrNull`: shared content parse, Int range-checked for int. */
std::optional<std::int64_t> kotlin_long(const std::string& text) {
    if (text.empty()) return std::nullopt;
    std::size_t at = 0;
    bool negative = false;
    if (text[0] == '+' || text[0] == '-') {
        negative = text[0] == '-';
        at = 1;
    }
    if (at == text.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; at < text.size(); ++at) {
        const char c = text[at];
        if (c < '0' || c > '9') return std::nullopt;
        const int digit = c - '0';
        if (value > (INT64_MAX - digit) / 10) return std::nullopt;  // Long overflow → null
        value = value * 10 + digit;
    }
    return negative ? -value : value;
}

std::optional<std::int32_t> kotlin_int(const std::string& text) {
    const std::optional<std::int64_t> value = kotlin_long(text);
    if (!value.has_value()) return std::nullopt;
    // Kotlin's intOrNull range-checks with its "… is not an Int" NumberFormatException → null.
    if (*value < -2147483648LL || *value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(*value);
}

/** `JsonObject.string(key)`: primitive STRING kind only (numbers and nulls fall through). */
std::optional<std::string> string_field(const json::Value& object, const std::string& key) {
    const json::Value* value = map_get(object, key);
    if (value == nullptr || value->kind != json::Kind::String) return std::nullopt;
    return value->text;
}

/** `?.trim()` (Kotlin String.trim — Unicode whitespace via the pinned helper). */
std::string trimmed(const std::string& text) { return json::trim(text); }

/** `takeIf { it.isNotEmpty() }` after a trim. */
std::optional<std::string> trimmed_nonempty(const std::string& text) {
    std::string result = json::trim(text);
    if (result.empty()) return std::nullopt;
    return result;
}

/** Kotlin String.equals(ignoreCase = true) over trimmed titles (ASCII case folding). */
bool equals_ignore_case(const std::string& left, const std::string& right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

}  // namespace

// ---- JSON envelopes -------------------------------------------------------------------------------

std::string AutomationToolProvider::error(const std::string& message) {
    return "Error: " + message;
}

json::Value AutomationToolProvider::task_json(const TaskInfo& task) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("id", json_string(task.id));
    members.emplace_back("name", json_string(task.name));
    members.emplace_back("prompt", json_string(task.prompt));
    if (task.model.has_value()) members.emplace_back("model", json_string(*task.model));
    if (task.system_prompt_id.has_value()) {
        members.emplace_back("system_prompt_id", json_string(*task.system_prompt_id));
    }
    members.emplace_back("cron", json_string(task.cron));
    members.emplace_back("enabled", json_literal(task.enabled ? "true" : "false"));
    members.emplace_back("created_at", json_literal(std::to_string(task.created_at)));
    if (task.last_run_at.has_value()) {
        members.emplace_back("last_run_at", json_literal(std::to_string(*task.last_run_at)));
    }
    members.emplace_back("next_run_at", json_literal(std::to_string(task.next_run_at)));
    return json_object(std::move(members));
}

json::Value AutomationToolProvider::loop_json(const LoopInfo& loop) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("conversation_id", json_string(loop.conversation_id));
    members.emplace_back("interval_seconds",
                         json_literal(std::to_string(loop.interval_ms / 1000)));
    if (loop.prompt.has_value()) members.emplace_back("prompt", json_string(*loop.prompt));
    members.emplace_back("next_fire_at", json_literal(std::to_string(loop.next_fire_at)));
    members.emplace_back("cycle_count", json_literal(std::to_string(loop.cycle_count)));
    if (loop.max_cycles.has_value()) {
        members.emplace_back("max_cycles", json_literal(std::to_string(*loop.max_cycles)));
    }
    members.emplace_back("active", json_literal(loop.active ? "true" : "false"));
    return json_object(std::move(members));
}

// ---- Provider -------------------------------------------------------------------------------------

AutomationToolProvider::AutomationToolProvider(TaskSeam& task_manager, LoopSeam& loop_manager,
                                               std::function<std::vector<SavedSystemPrompt>()>
                                                   saved_system_prompts,
                                               std::function<bool()> is_currently_enabled)
    : task_manager_(task_manager),
      loop_manager_(loop_manager),
      saved_system_prompts_(std::move(saved_system_prompts)),
      is_currently_enabled_(std::move(is_currently_enabled)) {}

bool AutomationToolProvider::handles(const std::string& name) const { return is_tool_name(name); }

std::vector<openai::ToolDefinition> AutomationToolProvider::definitions(
    const shell_provider::GenerationContext& ctx) const {
    if (!ctx.automation_tools_enabled) return {};
    std::vector<openai::ToolDefinition> definitions;

    {
        openai::ToolFunction function;
        function.name = kCreateTask;
        function.description =
            "Create an enabled background task with a 5-field cron schedule. Use only when the "
            "user explicitly asks to create a recurring task.";
        openai::ToolProperty name;
        name.type = "string";
        name.description = "A short, descriptive task name.";
        openai::ToolProperty prompt;
        prompt.type = "string";
        prompt.description = "The complete prompt to run on every occurrence.";
        openai::ToolProperty cron;
        cron.type = "string";
        cron.description =
            "A valid 5-field cron expression: minute hour day-of-month month day-of-week.";
        openai::ToolProperty model;
        model.type = "string";
        model.description = "Optional provider-prefixed model id. Omit to use the app default model.";
        openai::ToolProperty system_prompt;
        system_prompt.type = "string";
        system_prompt.description =
            "Optional saved system prompt, given as its id or its exact title. Omit to use the "
            "app default prompt.";
        function.parameters.properties = {
            {"name", std::move(name)},
            {"prompt", std::move(prompt)},
            {"cron", std::move(cron)},
            {"model", std::move(model)},
            {"system_prompt", std::move(system_prompt)},
        };
        function.parameters.required = {"name", "prompt", "cron"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kListTasks;
        function.description =
            "List all saved background tasks, including ids, schedules, enabled state, and next "
            "run times.";
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kDeleteTask;
        function.description =
            "Delete one saved task by exact id or unique task name. This is destructive; use "
            "only when the user explicitly asks.";
        openai::ToolProperty id_or_name;
        id_or_name.type = "string";
        id_or_name.description = "The exact task id or a unique task name.";
        function.parameters.properties = {{"id_or_name", std::move(id_or_name)}};
        function.parameters.required = {"id_or_name"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kStartLoop;
        // The bounds interpolate from loop_policy, so the schema copy tracks the policy itself.
        function.description =
            "Start a Loop in the current conversation. interval_seconds must be " +
            std::to_string(loop::kMinIntervalSeconds) + " to " +
            std::to_string(loop::kMaxIntervalSeconds) + ". max_cycles defaults to " +
            std::to_string(loop::kDefaultMaxCycles) + " and must be " +
            std::to_string(loop::kMinMaxCycles) + " to " + std::to_string(loop::kMaxMaxCycles) +
            ".";
        openai::ToolProperty interval_seconds;
        interval_seconds.type = "integer";
        interval_seconds.description =
            "Seconds between cycles, from " + std::to_string(loop::kMinIntervalSeconds) +
            " through " + std::to_string(loop::kMaxIntervalSeconds) + " (7 days).";
        openai::ToolProperty prompt;
        prompt.type = "string";
        prompt.description = "Optional prompt injected each cycle. Blank or omitted means Continue.";
        openai::ToolProperty max_cycles;
        max_cycles.type = "integer";
        max_cycles.description =
            "Optional safety limit from " + std::to_string(loop::kMinMaxCycles) + " through " +
            std::to_string(loop::kMaxMaxCycles) + ". Defaults to " +
            std::to_string(loop::kDefaultMaxCycles) + ".";
        function.parameters.properties = {
            {"interval_seconds", std::move(interval_seconds)},
            {"prompt", std::move(prompt)},
            {"max_cycles", std::move(max_cycles)},
        };
        function.parameters.required = {"interval_seconds"};
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    {
        openai::ToolFunction function;
        function.name = kStopLoop;
        function.description = "Stop the Loop attached to the current conversation.";
        definitions.push_back(openai::ToolDefinition{"function", std::move(function)});
    }
    return definitions;
}

std::string AutomationToolProvider::execute(const std::string& name, const std::string& arguments,
                                            const shell_provider::GenerationContext& ctx) {
    // The flag is re-checked at execution time (stale tool calls must not mutate state).
    if (!ctx.automation_tools_enabled || !is_currently_enabled_()) {
        return error("Automation tools are disabled");
    }
    if (!is_tool_name(name)) return error("Unknown automation tool: " + name);

    // Kotlin wraps every parse/dispatch exception; CancellationException has no equivalent here.
    try {
        const std::string args_text = json::is_blank(arguments) ? "{}" : arguments;
        const json::ParseResult parsed = json::parse(args_text);
        if (!parsed.value) {
            throw std::runtime_error("Unexpected JSON token at offset " +
                                     std::to_string(parsed.error_position) + ": " + parsed.error);
        }
        if (parsed.value->kind != json::Kind::Object) {
            throw std::runtime_error("Element " + json::to_kotlin_string(*parsed.value) +
                                     " is not a JsonObject");
        }
        const json::Value& args = *parsed.value;
        if (name == kCreateTask) return create_task(args);
        if (name == kListTasks) return list_tasks();
        if (name == kDeleteTask) return delete_task(args);
        if (name == kStartLoop) return start_loop(args, ctx);
        if (name == kStopLoop) return stop_loop(ctx);
        return error("Unknown automation tool: " + name);
    } catch (const std::exception& exception) {
        const std::string message = exception.what();
        return error(json::is_blank(message) ? "Invalid tool arguments" : message);
    }
}

std::string AutomationToolProvider::create_task(const json::Value& args) {
    const std::string name = trimmed(string_field(args, "name").value_or(""));
    if (name.empty()) return error("name is required");
    const std::string prompt = trimmed(string_field(args, "prompt").value_or(""));
    if (prompt.empty()) return error("prompt is required");
    const std::string cron = trimmed(string_field(args, "cron").value_or(""));
    if (cron.empty()) return error("cron is required");
    if (!agora::core::cron::CronExpression::is_valid(cron)) {
        return error("Invalid 5-field cron expression: " + cron);
    }

    std::optional<std::string> model;
    if (const auto value = string_field(args, "model")) model = trimmed_nonempty(*value);
    std::optional<std::string> system_prompt_id;
    if (const auto requested = string_field(args, "system_prompt")) {
        if (const std::optional<std::string> wanted = trimmed_nonempty(*requested)) {
            // Fail closed: silently falling back to the app default would hide the mismatch
            // until the task runs with the wrong prompt.
            const std::vector<SavedSystemPrompt> prompts = saved_system_prompts_();
            bool found_by_id = false;
            std::vector<const SavedSystemPrompt*> by_title;
            for (const SavedSystemPrompt& prompt_entry : prompts) {
                if (prompt_entry.id == *wanted) found_by_id = true;
                if (equals_ignore_case(json::trim(prompt_entry.title), *wanted)) {
                    by_title.push_back(&prompt_entry);
                }
            }
            if (found_by_id) {
                system_prompt_id = *wanted;
            } else if (by_title.size() == 1) {
                system_prompt_id = by_title.front()->id;
            } else if (by_title.size() > 1) {
                return error("Multiple saved system prompts are titled \"" + *wanted +
                             "\"; pass the prompt id instead");
            } else {
                return error("Saved system prompt not found: " + *wanted);
            }
        }
    }

    const TaskInfo task = task_manager_.create_task(name, prompt, cron, model, system_prompt_id);
    return render(json_object(
        {{ "type", json_string(kCreateTask) }, { "task", task_json(task) }}));
}

std::string AutomationToolProvider::list_tasks() {
    const std::vector<TaskInfo> tasks = task_manager_.list_tasks_snapshot();
    std::vector<json::Value> entries;
    entries.reserve(tasks.size());
    for (const TaskInfo& task : tasks) entries.push_back(task_json(task));
    return render(json_object({{ "type", json_string(kListTasks) },
                               { "tasks", json_array(std::move(entries)) }}));
}

std::string AutomationToolProvider::delete_task(const json::Value& args) {
    const std::string id_or_name = trimmed(string_field(args, "id_or_name").value_or(""));
    if (id_or_name.empty()) return error("id_or_name is required");

    const TaskSeam::DeleteOutcome outcome = task_manager_.delete_task_by_id_or_name(id_or_name);
    switch (outcome.kind) {
        case TaskSeam::DeleteOutcome::Kind::Deleted:
            return render(json_object({{ "type", json_string(kDeleteTask) },
                                       { "deleted", json_literal("true") },
                                       { "task", task_json(outcome.task) }}));
        case TaskSeam::DeleteOutcome::Kind::NotFound:
            return error("Task not found: " + id_or_name);
        case TaskSeam::DeleteOutcome::Kind::Ambiguous: {
            std::string matches;
            for (std::size_t i = 0; i < outcome.matches.size(); ++i) {
                if (i > 0) matches += ", ";
                matches += outcome.matches[i].name + " (" + outcome.matches[i].id + ")";
            }
            return error("Multiple tasks match '" + id_or_name + "': " + matches);
        }
    }
    return error("Invalid tool arguments");  // unreachable; keeps every path returning
}

std::string AutomationToolProvider::start_loop(const json::Value& args,
                                               const shell_provider::GenerationContext& ctx) {
    std::optional<std::string> conversation_id;
    if (ctx.conversation_id.has_value()) {
        conversation_id = trimmed_nonempty(*ctx.conversation_id);
    }
    if (!conversation_id.has_value()) {
        return error("start_loop requires a current conversation");
    }

    const std::optional<std::string> interval_text =
        primitive_content(map_get(args, "interval_seconds"));
    const std::optional<std::int64_t> interval_seconds =
        interval_text.has_value() ? kotlin_long(*interval_text) : std::nullopt;
    if (!interval_seconds.has_value()) return error("interval_seconds must be an integer");
    if (*interval_seconds < loop::kMinIntervalSeconds ||
        *interval_seconds > loop::kMaxIntervalSeconds) {
        return error("interval_seconds must be between " +
                     std::to_string(loop::kMinIntervalSeconds) + " and " +
                     std::to_string(loop::kMaxIntervalSeconds));
    }

    std::int32_t max_cycles = loop::kDefaultMaxCycles;
    if (map_get(args, "max_cycles") != nullptr) {
        // Key presence (even JsonNull) routes through int() and its Int-range parse.
        const std::optional<std::string> cycles_text = primitive_content(map_get(args, "max_cycles"));
        const std::optional<std::int32_t> parsed =
            cycles_text.has_value() ? kotlin_int(*cycles_text) : std::nullopt;
        if (!parsed.has_value()) return error("max_cycles must be an integer");
        max_cycles = *parsed;
    }
    if (max_cycles < loop::kMinMaxCycles || max_cycles > loop::kMaxMaxCycles) {
        return error("max_cycles must be between " + std::to_string(loop::kMinMaxCycles) +
                     " and " + std::to_string(loop::kMaxMaxCycles));
    }

    std::optional<std::string> prompt;
    if (const auto value = string_field(args, "prompt")) prompt = trimmed_nonempty(*value);

    const LoopSeam::StartOutcome outcome = loop_manager_.start_loop(
        *conversation_id, *interval_seconds * 1000, prompt, max_cycles);
    switch (outcome.kind) {
        case LoopSeam::StartOutcome::Kind::Started:
            return render(json_object({{ "type", json_string(kStartLoop) },
                                       { "status", json_string("started") },
                                       { "loop", loop_json(outcome.loop) }}));
        case LoopSeam::StartOutcome::Kind::Conflict:
            return error("A Loop is already active for this conversation (next fire at " +
                         std::to_string(outcome.existing.next_fire_at) + ")");
        case LoopSeam::StartOutcome::Kind::Invalid:
            return error(outcome.reason);
        case LoopSeam::StartOutcome::Kind::ConversationMissing:
            return error("Conversation not found: " + *conversation_id);
    }
    return error("Invalid tool arguments");  // unreachable; keeps every path returning
}

std::string AutomationToolProvider::stop_loop(const shell_provider::GenerationContext& ctx) {
    std::optional<std::string> conversation_id;
    if (ctx.conversation_id.has_value()) {
        conversation_id = trimmed_nonempty(*ctx.conversation_id);
    }
    if (!conversation_id.has_value()) {
        return error("stop_loop requires a current conversation");
    }

    const auto stop_json = [&](const char* status) {
        return render(json_object({{ "type", json_string(kStopLoop) },
                                   { "status", json_string(status) },
                                   { "conversation_id", json_string(*conversation_id) }}));
    };
    switch (loop_manager_.stop_loop(*conversation_id)) {
        case LoopSeam::StopKind::Stopped:
            return stop_json("stopped");
        case LoopSeam::StopKind::AlreadyStopped:
            return stop_json("already_stopped");
        case LoopSeam::StopKind::NotFound:
            return error("No Loop exists for conversation: " + *conversation_id);
    }
    return error("Invalid tool arguments");  // unreachable; keeps every path returning
}

}  // namespace agora::core::automation