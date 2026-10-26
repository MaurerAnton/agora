#include "agora/core/shell_durable_jobs.hpp"

#include <algorithm>
#include <set>
#include <utility>

#include "agora/core/java_utf8.hpp"
#include "agora/core/json.hpp"
#include "agora/core/utf16.hpp"

namespace agora::core::durable_jobs {
namespace {

namespace json = agora::core::json;

// ---------------------------------------------------------------------------------------------
// JSON builders (kotlinx buildJsonObject + JsonElement.toString() shape)
// ---------------------------------------------------------------------------------------------

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

json::Value jobj(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

/** Kotlin Long/Int.toString(): plain decimal, sign included. */
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

// ---------------------------------------------------------------------------------------------
// Java String.to*OrNull parsing (Integer.parseInt / Long.parseLong semantics)
// ---------------------------------------------------------------------------------------------

std::optional<std::int64_t> parse_signed(std::string_view text, std::uint64_t limit) {
    if (text.empty()) return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (text[0] == '-') {
        negative = true;
        index = 1;
    } else if (text[0] == '+') {
        index = 1;
    }
    if (index >= text.size()) return std::nullopt;
    std::uint64_t accumulator = 0;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') return std::nullopt;
        const std::uint64_t digit = static_cast<std::uint64_t>(c - '0');
        if (accumulator > (limit - digit) / 10) return std::nullopt;
        accumulator = accumulator * 10 + digit;
    }
    if (negative) return -static_cast<std::int64_t>(accumulator);
    return static_cast<std::int64_t>(accumulator);
}

std::optional<std::int32_t> to_int_or_null(std::string_view text) {
    const auto value = parse_signed(text, 2147483648ULL);  // |Int.MIN_VALUE|
    if (!value) return std::nullopt;
    if (value < -2147483648LL || value > 2147483647LL) return std::nullopt;
    return static_cast<std::int32_t>(*value);
}

std::optional<std::int64_t> to_long_or_null(std::string_view text) {
    return parse_signed(text, 9223372036854775808ULL);  // |Long.MIN_VALUE|
}

// ---------------------------------------------------------------------------------------------
// kotlinx JsonObject access
// ---------------------------------------------------------------------------------------------

/** Last member wins (LinkedHashMap decode: later duplicate overwrites the value). */
const json::Value* member_last(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

/** `as? JsonPrimitive` — quoted strings and unquoted literals (number/bool/null). */
bool is_primitive(const json::Value& value) {
    return value.kind == json::Kind::String || value.kind == json::Kind::Literal;
}

/** JsonPrimitive.content (decoded text for strings, raw text for literals). */
std::optional<std::string> primitive_content(const json::Value* value) {
    if (value == nullptr || !is_primitive(*value)) return std::nullopt;
    return value->text;
}

/** `JsonObject?.stringValue`: primitive + not blank. */
std::optional<std::string> string_value(const json::Value* object, std::string_view key) {
    if (object == nullptr || object->kind != json::Kind::Object) return std::nullopt;
    const auto content = primitive_content(member_last(*object, key));
    if (!content || json::is_blank(*content)) return std::nullopt;
    return content;
}

bool is_string_primitive(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::String;
}

/**
 * Kotlin String.lowercase() (locale-independent). The pinned scenario space uses ASCII
 * lifecycle states; non-ASCII code points are left unchanged, matching the JVM for them only
 * where its simple mapping agrees.
 */
std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

bool ascii_iequals(std::string_view left, std::string_view right) {
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

std::string join_names(const std::vector<std::string>& names) {
    std::string out;
    for (std::size_t i = 0; i < names.size(); ++i) {
        if (i > 0) out += ", ";
        out += names[i];
    }
    return out;
}

const std::set<std::string, std::less<>> kTerminalJobStates = {
    "succeeded", "failed", "stopped", "interrupted",
};

/** Both acknowledgement gates use the same four lifecycle states (two private Kotlin sets). */
bool is_terminal_ack_state(std::string_view lowercased) {
    return kTerminalJobStates.count(lowercased) > 0;
}

/** kotlinx put(JsonPrimitive) of a snapshot trio, used by the wait-loop JSON builders. */
void put_snapshot_fields(std::vector<std::pair<std::string, json::Value>>& members,
                         const JobOutputSnapshot& snapshot) {
    members.emplace_back("output", jstr(snapshot.text));
    members.emplace_back("output_bytes", jnum(i64_digits(snapshot.output_bytes)));
    members.emplace_back("truncated", jbool(snapshot.truncated));
}

std::int32_t poll_interval_after(std::int32_t interval) { return std::min(interval * 2, 5000); }

bool hook_active(const std::function<bool()>& is_active) {
    return is_active ? is_active() : true;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Device resolution + messages
// ---------------------------------------------------------------------------------------------

std::optional<Device> resolve_conch_device(std::string_view server_name,
                                           const std::vector<Device>& devices) {
    std::vector<const Device*> conch_devices;
    for (const Device& device : devices) {
        if (device.type != "ssh") conch_devices.push_back(&device);
    }
    if (!json::is_blank(server_name)) {
        for (const Device* device : conch_devices) {
            if (ascii_iequals(device->name, server_name)) return *device;
        }
        return std::nullopt;
    }
    if (conch_devices.size() == 1) return *conch_devices.front();
    return std::nullopt;
}

std::string conch_server_not_found_message(std::string_view server_name,
                                           const std::vector<Device>& devices) {
    std::vector<std::string> names;
    for (const Device& device : devices) {
        if (device.type == "ssh") continue;
        names.push_back(json::is_blank(device.name) ? "Untitled" : device.name);
    }
    if (names.empty()) {
        return "No Conch server is configured. Background jobs require Conch.";
    }
    if (!json::is_blank(server_name)) {
        return "Unknown Conch server \"" + std::string(server_name) + "\". Available: " +
               join_names(names) + ".";
    }
    if (names.size() > 1) {
        return "Multiple Conch servers are available. Specify one: " + join_names(names) + ".";
    }
    return "Conch server is unavailable.";
}

std::int32_t max_wait_ms(std::int64_t tool_timeout_ms) {
    const std::int64_t remaining = tool_timeout_ms - 5000;
    return static_cast<std::int32_t>(std::max<std::int64_t>(remaining, 1000));
}

// ---------------------------------------------------------------------------------------------
// Job payload rules
// ---------------------------------------------------------------------------------------------

bool is_terminal_job_payload(std::string_view raw) {
    if (json::is_blank(raw)) return false;
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return false;
    const auto state = primitive_content(member_last(*parsed.value, "state"));
    if (state) return is_terminal_ack_state(ascii_lower(*state));
    return member_last(*parsed.value, "error") != nullptr;
}

std::optional<JobOutputSnapshot> conch_job_output_snapshot(std::string_view raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return std::nullopt;
    const json::Value* output = member_last(*parsed.value, "output");
    if (!is_string_primitive(output)) return std::nullopt;
    const json::Value* output_bytes = member_last(*parsed.value, "output_bytes");
    if (output_bytes == nullptr || output_bytes->kind == json::Kind::String) return std::nullopt;
    const auto bytes = to_long_or_null(output_bytes->text);
    if (!bytes || *bytes < 0) return std::nullopt;

    // truncated: a non-string primitive parsed strictly, else outputBytes > UTF-8 size.
    std::optional<bool> explicit_flag;
    const json::Value* truncated = member_last(*parsed.value, "truncated");
    if (truncated != nullptr && truncated->kind != json::Kind::String) {
        if (truncated->text == "true") explicit_flag = true;
        else if (truncated->text == "false") explicit_flag = false;
    }
    const bool flag = explicit_flag.value_or(
        *bytes > static_cast<std::int64_t>(output->text.size()));
    return JobOutputSnapshot{output->text, *bytes, flag};
}

JobOutputUpdate ConchJobOutputCursor::consume(std::string_view raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) return {"", 0};
    const json::Value* output = member_last(*parsed.value, "output");
    if (!is_string_primitive(output)) return {"", 0};
    const json::Value* output_bytes = member_last(*parsed.value, "output_bytes");
    if (output_bytes == nullptr || output_bytes->kind == json::Kind::String) return {"", 0};
    const auto bytes = to_long_or_null(output_bytes->text);
    if (!bytes || *bytes < 0) return {"", 0};

    const std::string& retained = output->text;
    const std::int64_t retained_size = static_cast<std::int64_t>(retained.size());
    const std::int64_t retained_start = std::max<std::int64_t>(*bytes - retained_size, 0);
    const std::int64_t lost_bytes = std::max<std::int64_t>(retained_start - emitted_bytes_, 0);
    const std::int64_t start =
        std::min<std::int64_t>(std::max<std::int64_t>(emitted_bytes_ - retained_start, 0),
                               retained_size);

    // JVM: retained.copyOfRange(start, size).toString(UTF_8) — a REPLACE decode of the slice.
    const std::string slice = retained.substr(static_cast<std::size_t>(start));
    std::u16string decoded;
    java_utf8::decode_replace(slice, decoded);
    std::string delta = utf16::to_utf8(decoded);

    emitted_bytes_ = std::max<std::int64_t>(emitted_bytes_, *bytes);
    return {std::move(delta), lost_bytes};
}

std::vector<JobAcknowledgement> terminal_shell_job_acknowledgements(
    const std::vector<ToolCallRecord>& calls) {
    std::vector<JobAcknowledgement> acknowledgements;
    std::vector<std::pair<std::string, std::string>> seen;
    for (const ToolCallRecord& call : calls) {
        if (call.tool_name != "execute_shell_command" && call.tool_name != "get_shell_job" &&
            call.tool_name != "wait_for_job") {
            continue;
        }
        const json::ParseResult parsed = json::parse(call.result);
        if (!parsed.value || parsed.value->kind != json::Kind::Object) continue;
        const json::Value& envelope = *parsed.value;
        const json::Value* nested_value = member_last(envelope, "result");
        const json::Value* nested =
            (nested_value != nullptr && nested_value->kind == json::Kind::Object)
                ? nested_value
                : nullptr;

        std::optional<std::string> state = string_value(nested, "state");
        if (!state) state = string_value(&envelope, "state");
        if (!state || !is_terminal_ack_state(ascii_lower(*state))) continue;

        std::optional<std::string> job_id = string_value(&envelope, "job_id");
        if (!job_id) job_id = string_value(nested, "job_id");
        if (!job_id) continue;

        const json::ParseResult parsed_arguments = json::parse(call.arguments);
        const json::Value* arguments =
            (parsed_arguments.value && parsed_arguments.value->kind == json::Kind::Object)
                ? &*parsed_arguments.value
                : nullptr;
        std::string server = string_value(&envelope, "server")
                                 .value_or(string_value(nested, "server")
                                               .value_or(string_value(arguments, "server")
                                                             .value_or(std::string())));

        const auto key = std::make_pair(ascii_lower(server), *job_id);
        if (std::find(seen.begin(), seen.end(), key) != seen.end()) continue;
        seen.push_back(key);
        acknowledgements.push_back({std::move(server), *job_id});
    }
    return acknowledgements;
}

// ---------------------------------------------------------------------------------------------
// waitForShellJobPolling
// ---------------------------------------------------------------------------------------------

std::string wait_for_job_polling(const PollingParams& params, const PollingHooks& hooks) {
    const std::int64_t start = hooks.now();
    std::int32_t consecutive_failures = 0;
    std::optional<std::string> last_failure;
    std::int32_t poll_interval_ms = 500;
    std::optional<JobOutputSnapshot> latest_snapshot;
    std::optional<std::string> last_published;

    while (hook_active(hooks.is_active)) {
        std::optional<std::string> raw;
        try {
            raw = hooks.poll();
            consecutive_failures = 0;
        } catch (const JobCancelled&) {
            throw;
        } catch (const std::exception& failure) {
            ++consecutive_failures;
            last_failure = failure.what();
            if (consecutive_failures >= 5) {
                std::vector<std::pair<std::string, json::Value>> members;
                members.emplace_back("type", jstr("wait_for_job"));
                members.emplace_back("error", jstr("poll_failed"));
                members.emplace_back("message",
                                     jstr("Failed to poll job " +
                                          std::to_string(consecutive_failures) +
                                          " times in a row: " + last_failure.value_or("")));
                members.emplace_back("server", jstr(params.server_name));
                members.emplace_back("job_id", jstr(params.job_id));
                members.emplace_back("durable", jbool(true));
                members.emplace_back("state", jstr("unknown"));
                if (latest_snapshot) put_snapshot_fields(members, *latest_snapshot);
                members.emplace_back(
                    "note", jstr("The job may still be running. Retry with the same job_id; it "
                                 "was not stopped by this wait failure."));
                return json::to_kotlin_string(jobj(std::move(members)));
            }
            raw.reset();
        }

        if (raw) {
            if (const auto snapshot = conch_job_output_snapshot(*raw)) {
                latest_snapshot = *snapshot;
                if (!last_published || *last_published != snapshot->text) {
                    if (hooks.on_snapshot) hooks.on_snapshot(snapshot->text);
                    last_published = snapshot->text;
                }
            }
            if (is_terminal_job_payload(*raw)) {
                std::vector<std::pair<std::string, json::Value>> members;
                members.emplace_back("type", jstr("wait_for_job"));
                members.emplace_back("job_id", jstr(params.job_id));
                members.emplace_back("waited_ms", jnum(i64_digits(hooks.now() - start)));
                const json::ParseResult parsed = json::parse(*raw);
                if (parsed.value) members.emplace_back("result", *parsed.value);
                else members.emplace_back("result_raw", jstr(*raw));
                return json::to_kotlin_string(jobj(std::move(members)));
            }
        }

        const std::int64_t elapsed = hooks.now() - start;
        if (elapsed >= params.timeout_ms) {
            std::string note = "Job still running. Call wait_for_job again to keep waiting, or "
                               "get_shell_job for a one-shot look.";
            if (params.clamped_from) {
                note += " The requested timeout_ms=" + std::to_string(*params.clamped_from) +
                        " exceeded this tool call's ceiling of " +
                        std::to_string(params.ceiling_ms) +
                        "ms and was clamped, so the job has only been waited on for that long.";
            }
            std::vector<std::pair<std::string, json::Value>> members;
            members.emplace_back("type", jstr("wait_for_job"));
            members.emplace_back("job_id", jstr(params.job_id));
            members.emplace_back("state", jstr("running"));
            members.emplace_back("waited_ms", jnum(i64_digits(elapsed)));
            members.emplace_back("timed_out", jbool(true));
            if (latest_snapshot) put_snapshot_fields(members, *latest_snapshot);
            members.emplace_back("note", jstr(std::move(note)));
            return json::to_kotlin_string(jobj(std::move(members)));
        }

        const std::int32_t remaining = static_cast<std::int32_t>(params.timeout_ms - elapsed);
        hooks.delay(std::min<std::int32_t>(poll_interval_ms, remaining));
        poll_interval_ms = poll_interval_after(poll_interval_ms);
    }
    return shell_tools::json_error("wait_for_job", "cancelled", params.server_name, std::nullopt);
}

// ---------------------------------------------------------------------------------------------
// waitForShellJob (public entry)
// ---------------------------------------------------------------------------------------------

std::string wait_for_job(std::string_view arguments, const JobToolContext& ctx,
                         const std::function<DurableJobBackend&(const Device&)>& make_backend,
                         const DurableJobHooks& hooks, std::vector<std::string>& snapshots) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string job_id = shell_tools::arg(args, "job_id");
    if (json::is_blank(job_id)) {
        return shell_tools::json_error("wait_for_job", "job_id is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server = shell_tools::arg(args, "server");
    const std::string raw_timeout = shell_tools::arg(args, "timeout_ms");
    if (json::is_blank(raw_timeout)) {
        return shell_tools::json_error("wait_for_job", "timeout_ms is required", server,
                                       std::nullopt);
    }
    const auto requested = to_int_or_null(raw_timeout);
    if (!requested) {
        return shell_tools::json_error("wait_for_job",
                                       "timeout_ms must be an integer, got \"" + raw_timeout +
                                           "\"",
                                       server, std::nullopt);
    }
    const auto device = resolve_conch_device(server, ctx.devices);
    if (!device) {
        return shell_tools::json_error("wait_for_job",
                                       conch_server_not_found_message(server, ctx.devices),
                                       server, std::nullopt);
    }

    const std::int32_t ceiling_ms = max_wait_ms(ctx.tool_timeout_ms);
    const std::int32_t timeout_ms =
        std::max(std::min(*requested, ceiling_ms), 1000);  // coerceIn(1000, ceilingMs)
    const std::optional<std::int32_t> clamped_from =
        (*requested > ceiling_ms) ? std::optional<std::int32_t>(*requested) : std::nullopt;

    DurableJobBackend& backend = make_backend(*device);
    PollingParams params;
    params.job_id = job_id;
    params.server_name = backend.device_name();
    params.timeout_ms = timeout_ms;
    params.clamped_from = clamped_from;
    params.ceiling_ms = ceiling_ms;

    PollingHooks polling;
    polling.now = hooks.now;
    polling.delay = hooks.delay;
    polling.is_active = hooks.is_active;
    polling.poll = [&backend, &job_id] { return backend.get_job(job_id); };
    polling.on_snapshot = [&snapshots](const std::string& text) { snapshots.push_back(text); };

    std::string json;
    try {
        json = wait_for_job_polling(params, polling);
    } catch (...) {
        backend.close();  // Kotlin `finally { backend.close() }`
        throw;
    }
    backend.close();
    return json;
}

// ---------------------------------------------------------------------------------------------
// executeDurableForeground
// ---------------------------------------------------------------------------------------------

DurableForegroundOutcome execute_durable_foreground(
    std::string_view command, std::string_view workdir, std::int32_t wait_ms,
    DurableJobBackend& backend, const DurableJobHooks& hooks) {
    const std::string command_text(command);
    std::string start_result;
    try {
        start_result = backend.start_job(command_text, std::string(workdir), 86400000);
    } catch (const JobCancelled&) {
        throw;
    } catch (const std::exception& failure) {
        // Kotlin: `e.message ?: "Failed to start durable foreground job"` — a C++ exception
        // always carries what(); scripted failures always carry the intended message.
        return {shell_tools::json_error("execute_shell_command", failure.what(),
                                        backend.device_name(), command_text),
                false, {}};
    }

    const json::ParseResult parsed_start = json::parse(start_result);
    if (!parsed_start.value || parsed_start.value->kind != json::Kind::Object) {
        return {start_result, false, {}};  // unparseable/non-object: verbatim passthrough
    }
    if (member_last(*parsed_start.value, "error") != nullptr) {
        return {start_result, false, {}};  // error payload: verbatim passthrough
    }
    const auto job_id = string_value(&*parsed_start.value, "job_id");
    if (!job_id) {
        return {shell_tools::json_error(
                    "execute_shell_command", "Conch started a job without returning job_id",
                    backend.device_name(), command_text),
                false, {}};
    }

    // Output collection lives outside the loop's exception scope: Kotlin's driver collects via
    // an external callback, so deltas emitted before a cancellation stay visible.
    ConchJobOutputCursor cursor;
    std::vector<std::string> outputs;
    try {
        const std::int64_t start = hooks.now();
        std::int32_t poll_interval_ms = 500;
        std::int32_t consecutive_failures = 0;
        std::optional<std::string> last_failure;

        while (hook_active(hooks.is_active)) {
            std::optional<std::string> raw;
            try {
                raw = backend.get_job(*job_id);
                consecutive_failures = 0;
            } catch (const JobCancelled&) {
                throw;  // → outer catch stops the remote job
            } catch (const std::exception& failure) {
                ++consecutive_failures;
                last_failure = failure.what();
                if (consecutive_failures >= 5) {
                    std::vector<std::pair<std::string, json::Value>> members;
                    members.emplace_back("type", jstr("execute_shell_command"));
                    members.emplace_back("error", jstr("poll_failed"));
                    members.emplace_back("message",
                                         jstr("Durable job could not be polled " +
                                              std::to_string(consecutive_failures) +
                                              " times: " + *last_failure));
                    members.emplace_back("server", jstr(backend.device_name()));
                    members.emplace_back("command", jstr(command_text));
                    members.emplace_back("job_id", jstr(*job_id));
                    members.emplace_back("durable", jbool(true));
                    members.emplace_back("state", jstr("unknown"));
                    members.emplace_back(
                        "note", jstr("The command may still be running. Keep this job_id and "
                                     "retry with wait_for_job or get_shell_job; it was not "
                                     "killed or restarted."));
                    return {json::to_kotlin_string(jobj(std::move(members))), false,
                            std::move(outputs)};
                }
                raw.reset();
            }

            if (raw) {
                const JobOutputUpdate update = cursor.consume(*raw);
                if (update.lost_bytes > 0) {
                    outputs.push_back("[Conch output gap: " + std::to_string(update.lost_bytes) +
                                      " earlier UTF-8 bytes were evicted before Agora could read "
                                      "them.]\n");
                }
                if (!update.delta.empty()) outputs.push_back(update.delta);
                if (is_terminal_job_payload(*raw)) {
                    std::vector<std::pair<std::string, json::Value>> members;
                    members.emplace_back("type", jstr("execute_shell_command"));
                    members.emplace_back("server", jstr(backend.device_name()));
                    members.emplace_back("command", jstr(command_text));
                    members.emplace_back("job_id", jstr(*job_id));
                    members.emplace_back("durable", jbool(true));
                    const json::ParseResult parsed = json::parse(*raw);
                    if (parsed.value) members.emplace_back("result", *parsed.value);
                    else members.emplace_back("result_raw", jstr(*raw));
                    return {json::to_kotlin_string(jobj(std::move(members))), false,
                            std::move(outputs)};
                }
            }

            const std::int64_t elapsed = hooks.now() - start;
            if (elapsed >= wait_ms) {
                std::vector<std::pair<std::string, json::Value>> members;
                members.emplace_back("type", jstr("execute_shell_command"));
                members.emplace_back("server", jstr(backend.device_name()));
                members.emplace_back("command", jstr(command_text));
                members.emplace_back("job_id", jstr(*job_id));
                members.emplace_back("background", jbool(true));
                members.emplace_back("state", jstr("running"));
                members.emplace_back("waited_ms", jnum(i64_digits(elapsed)));
                members.emplace_back(
                    "note", jstr("Foreground wait expired; the same durable job is still "
                                 "running. Use wait_for_job to await it. The command was not "
                                 "killed or restarted."));
                return {json::to_kotlin_string(jobj(std::move(members))), false,
                        std::move(outputs)};
            }

            // kotlinx delay is cancellable: a cancelled coroutine throws here (Kotlin's
            // `catch (cancelled)` around the loop stops the remote job, then rethrows).
            if (!hook_active(hooks.is_active)) throw JobCancelled("cancelled");

            const std::int32_t remaining = static_cast<std::int32_t>(wait_ms - elapsed);
            hooks.delay(std::min<std::int32_t>(poll_interval_ms, remaining));
            poll_interval_ms = poll_interval_after(poll_interval_ms);
        }

        // Loop exited at the top: the coroutine was cancelled (Kotlin's post-loop message).
        return {shell_tools::json_error(
                    "execute_shell_command",
                    "cancelled while durable job " + *job_id + " continues on " +
                        backend.device_name(),
                    backend.device_name(), command_text),
                false, std::move(outputs)};
    } catch (const JobCancelled&) {
        try {
            backend.stop_job(*job_id, 5000);  // DURABLE_JOB_CANCELLATION_STOP_TIMEOUT_MS
        } catch (...) {
            // Kotlin runCatching { backend.stopJob(...) }
        }
        return {"", true, std::move(outputs)};
    }
}

// ---------------------------------------------------------------------------------------------
// listShellJobs / getShellJob
// ---------------------------------------------------------------------------------------------

std::string list_shell_jobs(std::string_view arguments, const JobToolContext& ctx,
                            const std::function<DurableJobBackend&(const Device&)>& make_backend) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string server = shell_tools::arg(args, "server");
    const auto device = resolve_conch_device(server, ctx.devices);
    if (!device) {
        return shell_tools::json_error("list_shell_jobs",
                                       conch_server_not_found_message(server, ctx.devices),
                                       server, std::nullopt);
    }
    DurableJobBackend& backend = make_backend(*device);
    try {
        const std::string result = backend.list_jobs();
        backend.close();
        return result;
    } catch (const JobCancelled&) {
        backend.close();
        throw;
    } catch (const std::exception& failure) {
        backend.close();
        // Kotlin: `e.message ?: "Failed to list shell jobs"` (C++ what() is never null).
        return shell_tools::json_error("list_shell_jobs", failure.what(), backend.device_name(),
                                       std::nullopt);
    }
}

std::string get_shell_job(std::string_view arguments, const JobToolContext& ctx,
                          const std::function<DurableJobBackend&(const Device&)>& make_backend) {
    const auto args = shell_tools::parse_tool_args(arguments);
    const std::string job_id = shell_tools::arg(args, "job_id");
    if (json::is_blank(job_id)) {
        return shell_tools::json_error("get_shell_job", "job_id is required", std::nullopt,
                                       std::nullopt);
    }
    const std::string server = shell_tools::arg(args, "server");
    const auto device = resolve_conch_device(server, ctx.devices);
    if (!device) {
        return shell_tools::json_error("get_shell_job",
                                       conch_server_not_found_message(server, ctx.devices),
                                       server, std::nullopt);
    }
    DurableJobBackend& backend = make_backend(*device);
    try {
        const std::string result = backend.get_job(job_id);
        backend.close();
        return result;
    } catch (const JobCancelled&) {
        backend.close();
        throw;
    } catch (const std::exception& failure) {
        backend.close();
        // Kotlin: `e.message ?: "Failed to get shell job"` (C++ what() is never null).
        return shell_tools::json_error("get_shell_job", failure.what(), backend.device_name(),
                                       std::nullopt);
    }
}

}  // namespace agora::core::durable_jobs
