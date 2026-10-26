// Golden parity test for ShellDurableJobExecutor.kt (the Conch durable-job orchestration).
//
// In the stand, ShellDurableJobExecutor.kt and ShellToolJson.kt compile VERBATIM over the
// platform/ leaf doubles; this side replays every frozen scenario through the C++ port and
// compares the canonical trace byte for byte. The scripted backend mirrors the stand double
// exactly — event recording, script exhaustion, and cancellation steps included — and the
// coroutine mechanics are supplied through the port's now/delay/is_active hooks (virtual clock
// for waitForShellJobPolling, the real clock for the public paths, where waited_ms is masked
// on both sides).
#include <algorithm>
#include <chrono>
#include <filesystem>
#include <fstream>
#include <map>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <thread>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_durable_jobs.hpp"
#include "agora/core/shell_tools.hpp"
#include "check.hpp"

namespace {

namespace durable = agora::core::durable_jobs;
namespace json = agora::core::json;
namespace shell_tools = agora::core::shell_tools;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

// ---- JSON builders (mirror of the driver's kotlinx buildJsonObject usage) -----------------------

json::Value jstr(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value jnum(std::string digits) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(digits);
    return value;
}

json::Value jbool(bool flag) { return jnum(flag ? "true" : "false"); }

json::Value jnull() { return jnum("null"); }

json::Value jobj(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

json::Value jarr(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

std::string i64_digits(std::int64_t value) {
    if (value == 0) return "0";
    bool negative = value < 0;
    std::uint64_t magnitude = negative ? (~static_cast<std::uint64_t>(value) + 1)
                                       : static_cast<std::uint64_t>(value);
    std::string digits;
    while (magnitude > 0) {
        digits.push_back(static_cast<char>('0' + magnitude % 10));
        magnitude /= 10;
    }
    if (negative) digits.push_back('-');
    std::reverse(digits.begin(), digits.end());
    return digits;
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

std::string required_string(const json::Value& object, std::string_view name,
                            const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || (value->kind != json::Kind::String && value->kind != json::Kind::Literal)) {
        scenario_fail(scenario, "missing string " + std::string(name));
    }
    return value->text;
}

std::optional<std::int64_t> optional_integer(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == json::Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != json::Kind::Literal) return std::nullopt;
    try {
        return std::stoll(value->text);
    } catch (...) {
        return std::nullopt;
    }
}

// ---- Scenario plumbing --------------------------------------------------------------------------

std::vector<durable::Device> devices_of(const json::Value& scenario, const std::string& stem) {
    std::vector<durable::Device> devices;
    const json::Value* list = field(scenario, "devices");
    if (list == nullptr) return devices;
    if (list->kind != json::Kind::Array) scenario_fail(stem, "devices is not an array");
    for (const json::Value& element : list->elements) {
        if (element.kind != json::Kind::Object) scenario_fail(stem, "device is not an object");
        durable::Device device;
        device.name = required_string(element, "name", stem);
        device.type = required_string(element, "type", stem);
        devices.push_back(std::move(device));
    }
    return devices;
}

durable::JobToolContext context_of(const json::Value& scenario, const std::string& stem) {
    durable::JobToolContext ctx;
    ctx.devices = devices_of(scenario, stem);
    ctx.tool_timeout_ms = optional_integer(scenario, "tool_timeout_ms").value_or(60'000);
    return ctx;
}

// ---- Scripted backend (mirrors platform/ConchBackend.kt) ----------------------------------------

struct ScriptStep {
    enum class Kind { Emit, Fail, CancelEmit, CancelFail } kind = Kind::Emit;
    std::string text;
};

class ScriptedBackend final : public durable::DurableJobBackend {
public:
    std::vector<json::Value> events;
    std::string device_name_ = "home";
    bool* cancelled_flag = nullptr;
    std::vector<ScriptStep> start_steps;
    std::vector<ScriptStep> poll_steps;
    std::vector<ScriptStep> list_steps;
    std::vector<ScriptStep> stop_steps;
    std::optional<ScriptStep> start_repeat;
    std::optional<ScriptStep> poll_repeat;
    std::optional<ScriptStep> list_repeat;
    std::optional<ScriptStep> stop_repeat;

    const std::string& device_name() const noexcept override { return device_name_; }

    std::string start_job(const std::string& command, const std::string& workdir,
                          std::int32_t timeout_ms) override {
        events.push_back(jobj({{"op", jstr("start")},
                               {"command", jstr(command)},
                               {"workdir", jstr(workdir)},
                               {"timeout_ms", jnum(std::to_string(timeout_ms))}}));
        return next_run("start");
    }

    std::string get_job(const std::string& job_id) override {
        events.push_back(jobj({{"op", jstr("get")}, {"job_id", jstr(job_id)}}));
        return next_run("poll");
    }

    std::string list_jobs() override {
        events.push_back(jobj({{"op", jstr("list")}}));
        return next_run("list");
    }

    std::string stop_job(const std::string& job_id,
                         std::optional<std::int64_t> call_timeout_ms) override {
        json::Value timeout =
            call_timeout_ms ? jnum(i64_digits(*call_timeout_ms)) : jnull();
        events.push_back(jobj({{"op", jstr("stop")},
                               {"job_id", jstr(job_id)},
                               {"timeout_ms", std::move(timeout)}}));
        return next_run("stop");
    }

    void close() override { events.push_back(jobj({{"op", jstr("close")}})); }

    /** Script consumption without event recording (the polling op's direct ShellJobPoller). */
    std::string next_run(const std::string& kind) {
        const ScriptStep step = next(kind);
        switch (step.kind) {
            case ScriptStep::Kind::Emit:
                return step.text;
            case ScriptStep::Kind::Fail:
                throw durable::PollFailure(step.text);
            case ScriptStep::Kind::CancelEmit:
                if (cancelled_flag != nullptr) *cancelled_flag = true;
                return step.text;
            case ScriptStep::Kind::CancelFail:
                if (cancelled_flag != nullptr) *cancelled_flag = true;
                throw durable::JobCancelled(step.text);
        }
        throw std::logic_error("unreachable");
    }

private:
    std::map<std::string, int> cursors_;

    ScriptStep next(const std::string& kind) {
        const std::vector<ScriptStep>* steps = nullptr;
        const std::optional<ScriptStep>* repeat = nullptr;
        if (kind == "start") {
            steps = &start_steps;
            repeat = &start_repeat;
        } else if (kind == "poll") {
            steps = &poll_steps;
            repeat = &poll_repeat;
        } else if (kind == "list") {
            steps = &list_steps;
            repeat = &list_repeat;
        } else if (kind == "stop") {
            steps = &stop_steps;
            repeat = &stop_repeat;
        } else {
            scenario_fail(kind, "unknown script kind");
        }
        int& cursor = cursors_[kind];
        ScriptStep step;
        if (cursor < static_cast<int>(steps->size())) {
            step = (*steps)[static_cast<std::size_t>(cursor)];
        } else if (repeat->has_value()) {
            step = **repeat;
        } else {
            step.kind = ScriptStep::Kind::Fail;
            step.text = kind + " script exhausted";
        }
        ++cursor;
        return step;
    }
};

ScriptStep parse_step(const json::Value& object, const std::string& stem) {
    if (object.kind != json::Kind::Object) scenario_fail(stem, "step is not an object");
    ScriptStep step;
    if (field(object, "ok") != nullptr) {
        step.kind = ScriptStep::Kind::Emit;
        step.text = required_string(object, "ok", stem);
    } else if (field(object, "throw") != nullptr) {
        step.kind = ScriptStep::Kind::Fail;
        step.text = required_string(object, "throw", stem);
    } else if (field(object, "cancel") != nullptr) {
        step.kind = ScriptStep::Kind::CancelEmit;
        step.text = required_string(object, "cancel", stem);
    } else if (field(object, "cancel_throw") != nullptr) {
        step.kind = ScriptStep::Kind::CancelFail;
        step.text = required_string(object, "cancel_throw", stem);
    } else {
        scenario_fail(stem, "step needs ok/throw/cancel/cancel_throw");
    }
    return step;
}

void install_script(const json::Value& scenario, ScriptedBackend& backend,
                    const std::string& stem) {
    auto single = [&](std::string_view key) -> std::vector<ScriptStep> {
        const json::Value* value = field(scenario, key);
        if (value == nullptr) return {};
        return {parse_step(*value, stem)};
    };
    auto array = [&](std::string_view key) -> std::vector<ScriptStep> {
        const json::Value* value = field(scenario, key);
        if (value == nullptr) return {};
        if (value->kind != json::Kind::Array) scenario_fail(stem, "steps are not an array");
        std::vector<ScriptStep> steps;
        for (const json::Value& element : value->elements) {
            steps.push_back(parse_step(element, stem));
        }
        return steps;
    };
    auto repeat = [&](std::string_view key) -> std::optional<ScriptStep> {
        const json::Value* value = field(scenario, key);
        if (value == nullptr) return std::nullopt;
        return parse_step(*value, stem);
    };
    backend.start_steps = single("start");
    backend.poll_steps = array("polls");
    backend.list_steps = single("list");
    backend.stop_steps =
        field(scenario, "stop") != nullptr ? single("stop")
                                           : std::vector<ScriptStep>{[] {
                                                 ScriptStep step;
                                                 step.kind = ScriptStep::Kind::Emit;
                                                 step.text = "{}";
                                                 return step;
                                             }()};
    backend.start_repeat = repeat("start_repeat");
    backend.poll_repeat = repeat("poll_repeat");
    backend.list_repeat = repeat("list_repeat");
    backend.stop_repeat = repeat("stop_repeat");
}

json::Value events_json(const ScriptedBackend& backend) {
    return jarr(backend.events);
}

// ---- Real-clock hooks (the public paths use System.currentTimeMillis + kotlinx delay) -----------

std::int64_t real_now_ms() {
    return std::chrono::duration_cast<std::chrono::milliseconds>(
               std::chrono::system_clock::now().time_since_epoch())
        .count();
}

void real_delay_ms(std::int64_t ms) {
    if (ms > 0) std::this_thread::sleep_for(std::chrono::milliseconds(ms));
}

/** The result-string mask the driver applies to the real-clock paths. */
std::string mask_waited_ms(std::string_view text) {
    const std::string pattern = "\"waited_ms\":";
    std::string out;
    std::size_t pos = 0;
    while (true) {
        const std::size_t found = text.find(pattern, pos);
        if (found == std::string_view::npos) {
            out.append(text.substr(pos));
            return out;
        }
        out.append(text.substr(pos, found + pattern.size() - pos));
        const std::size_t digits_start = found + pattern.size();
        std::size_t digits_end = digits_start;
        while (digits_end < text.size() && text[digits_end] >= '0' && text[digits_end] <= '9') {
            ++digits_end;
        }
        if (digits_end == digits_start) {
            // The Kotlin regex needs at least one digit; keep scanning after the colon.
            pos = digits_start;
            continue;
        }
        out += "MASKED";
        pos = digits_end;
    }
}

// ---- Op dispatch --------------------------------------------------------------------------------

json::Value run_op(const json::Value& scenario, const std::string& op, const std::string& stem) {
    if (op == "resolve_conch_device") {
        const auto device =
            durable::resolve_conch_device(required_string(scenario, "server", stem),
                                          devices_of(scenario, stem));
        if (!device) return jobj({{"device", jnull()}});
        return jobj({{"device", jobj({{"name", jstr(device->name)},
                                      {"type", jstr(device->type)}})}});
    }
    if (op == "conch_not_found") {
        return jobj({{"message", jstr(durable::conch_server_not_found_message(
                                     required_string(scenario, "server", stem),
                                     devices_of(scenario, stem)))}});
    }
    if (op == "is_terminal_payload") {
        return jobj({{"terminal",
                      jbool(durable::is_terminal_job_payload(
                          required_string(scenario, "raw", stem)))}});
    }
    if (op == "max_wait_ms") {
        const auto timeout = optional_integer(scenario, "tool_timeout_ms");
        if (!timeout) scenario_fail(stem, "missing tool_timeout_ms");
        return jobj({{"max_wait_ms", jnum(std::to_string(durable::max_wait_ms(*timeout)))}});
    }
    if (op == "output_snapshot") {
        const auto snapshot =
            durable::conch_job_output_snapshot(required_string(scenario, "raw", stem));
        if (!snapshot) return jobj({{"snapshot", jnull()}});
        return jobj({{"snapshot", jobj({{"text", jstr(snapshot->text)},
                                        {"output_bytes", jnum(i64_digits(snapshot->output_bytes))},
                                        {"truncated", jbool(snapshot->truncated)}})}});
    }
    if (op == "output_cursor") {
        durable::ConchJobOutputCursor cursor;
        const json::Value* payloads = field(scenario, "payloads");
        if (payloads == nullptr || payloads->kind != json::Kind::Array) {
            scenario_fail(stem, "missing payloads array");
        }
        std::vector<json::Value> updates;
        for (const json::Value& payload : payloads->elements) {
            if (payload.kind != json::Kind::String) scenario_fail(stem, "payload is not a string");
            const durable::JobOutputUpdate update = cursor.consume(payload.text);
            updates.push_back(jobj({{"delta", jstr(update.delta)},
                                    {"lost_bytes", jnum(i64_digits(update.lost_bytes))}}));
        }
        return jobj({{"updates", jarr(std::move(updates))}});
    }
    if (op == "wait_for_job_polling") {
        bool cancelled = false;
        std::int64_t clock = optional_integer(scenario, "start_clock").value_or(0);
        int delay_count = 0;
        const auto cancel_after_delay = optional_integer(scenario, "cancel_after_delay");

        ScriptedBackend backend;
        backend.cancelled_flag = &cancelled;
        install_script(scenario, backend, stem);

        durable::PollingParams params;
        params.job_id = required_string(scenario, "job_id", stem);
        params.server_name = required_string(scenario, "server", stem);
        params.timeout_ms = static_cast<std::int32_t>(
            optional_integer(scenario, "timeout_ms").value_or(0));
        params.clamped_from = optional_integer(scenario, "clamped_from")
                                  ? std::optional<std::int32_t>(static_cast<std::int32_t>(
                                        *optional_integer(scenario, "clamped_from")))
                                  : std::nullopt;
        params.ceiling_ms = static_cast<std::int32_t>(
            optional_integer(scenario, "ceiling_ms").value_or(0));

        std::vector<std::string> snapshots;
        durable::PollingHooks hooks;
        hooks.now = [&clock] { return clock; };
        hooks.delay = [&](std::int64_t ms) {
            clock += ms;
            ++delay_count;
            if (cancel_after_delay && delay_count == *cancel_after_delay) cancelled = true;
        };
        hooks.is_active = [&cancelled] { return !cancelled; };
        hooks.poll = [&backend] { return backend.next_run("poll"); };
        hooks.on_snapshot = [&snapshots](const std::string& text) {
            snapshots.push_back(text);
        };

        std::optional<std::string> result;
        try {
            result = durable::wait_for_job_polling(params, hooks);
        } catch (const durable::JobCancelled&) {
            result.reset();
        }

        std::vector<json::Value> snapshot_values;
        for (const std::string& text : snapshots) snapshot_values.push_back(jstr(text));
        json::Value result_value = result ? jstr(*result) : jnull();
        return jobj({{"result", std::move(result_value)},
                     {"snapshots", jarr(std::move(snapshot_values))},
                     {"clock", jnum(i64_digits(clock))}});
    }
    if (op == "wait_for_job") {
        bool cancelled = false;
        ScriptedBackend backend;
        backend.cancelled_flag = &cancelled;
        install_script(scenario, backend, stem);
        const durable::JobToolContext ctx = context_of(scenario, stem);
        durable::DurableJobHooks hooks;
        hooks.now = real_now_ms;
        hooks.delay = real_delay_ms;
        hooks.is_active = [&cancelled] { return !cancelled; };

        std::vector<std::string> snapshots;
        std::optional<std::string> result;
        try {
            result = durable::wait_for_job(
                required_string(scenario, "arguments", stem), ctx,
                [&backend](const durable::Device& device) -> durable::DurableJobBackend& {
                    backend.device_name_ = device.name;
                    return backend;
                },
                hooks, snapshots);
        } catch (const durable::JobCancelled&) {
            result.reset();
        }

        std::vector<json::Value> snapshot_values;
        for (const std::string& text : snapshots) snapshot_values.push_back(jstr(text));
        json::Value result_value = result ? jstr(mask_waited_ms(*result)) : jnull();
        return jobj({{"result", std::move(result_value)},
                     {"snapshots", jarr(std::move(snapshot_values))}});
    }
    if (op == "durable_foreground") {
        bool cancelled = false;
        ScriptedBackend backend;
        backend.cancelled_flag = &cancelled;
        install_script(scenario, backend, stem);

        const std::string server = required_string(scenario, "server", stem);
        const auto device =
            durable::resolve_conch_device(server, context_of(scenario, stem).devices);
        if (!device) scenario_fail(stem, "durable scenario must resolve a Conch device");
        backend.device_name_ = device->name;

        durable::DurableJobHooks hooks;
        hooks.now = real_now_ms;
        hooks.delay = real_delay_ms;
        hooks.is_active = [&cancelled] { return !cancelled; };

        std::optional<std::string> result;
        bool was_cancelled = false;
        std::vector<std::string> outputs;
        try {
            durable::DurableForegroundOutcome outcome = durable::execute_durable_foreground(
                required_string(scenario, "command", stem),
                required_string(scenario, "workdir", stem),
                static_cast<std::int32_t>(optional_integer(scenario, "wait_ms").value_or(0)),
                backend, hooks);
            was_cancelled = outcome.cancelled;
            outputs = std::move(outcome.outputs);
            if (!outcome.json.empty()) result = mask_waited_ms(outcome.json);
        } catch (const durable::JobCancelled&) {
            was_cancelled = true;
        }

        json::Value result_value = result ? jstr(*result) : jnull();
        std::vector<json::Value> output_values;
        for (const std::string& text : outputs) output_values.push_back(jstr(text));
        return jobj({{"result", std::move(result_value)},
                     {"cancelled", jbool(was_cancelled)},
                     {"outputs", jarr(std::move(output_values))},
                     {"events", events_json(backend)}});
    }
    if (op == "list_jobs" || op == "get_jobs") {
        bool cancelled = false;
        ScriptedBackend backend;
        backend.cancelled_flag = &cancelled;
        install_script(scenario, backend, stem);
        const durable::JobToolContext ctx = context_of(scenario, stem);
        durable::DurableJobHooks hooks;
        hooks.now = real_now_ms;
        hooks.delay = real_delay_ms;
        hooks.is_active = [&cancelled] { return !cancelled; };

        std::optional<std::string> result;
        const std::string arguments = required_string(scenario, "arguments", stem);
        const auto make_backend = [&backend](const durable::Device& device)
            -> durable::DurableJobBackend& {
            backend.device_name_ = device.name;
            return backend;
        };
        try {
            if (op == "list_jobs") {
                result = durable::list_shell_jobs(arguments, ctx, make_backend);
            } else {
                result = durable::get_shell_job(arguments, ctx, make_backend);
            }
        } catch (const durable::JobCancelled&) {
            result.reset();
        }
        json::Value result_value = result ? jstr(*result) : jnull();
        return jobj({{"result", std::move(result_value)}, {"events", events_json(backend)}});
    }
    if (op == "terminal_acks") {
        const json::Value* calls = field(scenario, "calls");
        if (calls == nullptr || calls->kind != json::Kind::Array) {
            scenario_fail(stem, "missing calls array");
        }
        std::vector<durable::ToolCallRecord> records;
        for (const json::Value& call : calls->elements) {
            durable::ToolCallRecord record;
            record.tool_name = required_string(call, "tool_name", stem);
            record.arguments = required_string(call, "arguments", stem);
            record.result = required_string(call, "result", stem);
            records.push_back(std::move(record));
        }
        std::vector<json::Value> acks;
        for (const durable::JobAcknowledgement& ack :
             durable::terminal_shell_job_acknowledgements(records)) {
            acks.push_back(jobj({{"server_name", jstr(ack.server_name)},
                                 {"job_id", jstr(ack.job_id)}}));
        }
        return jobj({{"acks", jarr(std::move(acks))}});
    }
    scenario_fail(stem, "unknown op: " + op);
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(160, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(shell_durable_jobs_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_DURABLE_JOBS_DIR;
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
    for (const std::filesystem::path& scenario_path : scenarios) {
        const std::string stem = scenario_path.stem().string();
        const json::ParseResult parsed = json::parse(read_file(scenario_path));
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            scenario_fail(stem, "scenario is not a JSON object");
        }
        const std::string op = required_string(*parsed.value, "op", stem);
        const json::Value outcome = run_op(*parsed.value, op, stem);
        const json::Value trace = jobj({{"op", jstr(op)},
                                        {"scenario", *parsed.value},
                                        {"outcome", outcome}});
        const std::string actual = json::to_kotlin_string(trace) + "\n";
        const std::string expected = read_file(golden_dir / (stem + ".trace"));
        CHECK(!expected.empty());
        if (actual != expected) {
            throw agora::core::test::Failure(stem + ": " + describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
