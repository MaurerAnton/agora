// Golden parity test for data/NativeRunArchivePolicy.kt.
//
// Replays every scenario under core/tools/golden/native-run-archive/scenarios through
// agora::core::native_run_archive and requires the produced trace to equal the frozen Kotlin trace
// byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/native-run-archive/NativeRunArchiveTraceDump.kt).
//
// The pins that matter: terminalization of every stored status (and the FAILED fallback for an
// invalid/absent one), PROCESS_RECOVERED for nonterminal rows, the end-reason fallback by terminal
// status, the checkpoint/endedAt max rules and the pass clamp; ownership uniqueness over message
// ids and (runId, sequence); and the parent-before-child order with missing-parent/cycle ambiguity,
// timestamp/id ties and LinkedHashMap duplicate-id resolution.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/native_run_archive.hpp"
#include "check.hpp"

namespace {

namespace archive = agora::core::native_run_archive;
namespace json = agora::core::json;
using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** Field-safe escaping: no raw separators or control characters survive. */
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '=': out += "\\e"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

/** `n` for a null value, `s:` + escaped text for a real one: the two can never collide. */
std::string encode_text(const std::optional<std::string>& value) {
    return value.has_value() ? "s:" + esc(*value) : "n";
}

std::string encode_long(const std::optional<std::int64_t>& value) {
    return value.has_value() ? "l:" + std::to_string(*value) : "n";
}

std::string encode_int(const std::optional<int>& value) {
    return value.has_value() ? "i:" + std::to_string(*value) : "n";
}

std::string encode_bool(bool value) { return value ? "true" : "false"; }

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

bool is_null(const Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

std::optional<std::string> optional_text(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string text(const Value& object, std::string_view name, std::string fallback = {}) {
    return optional_text(object, name).value_or(std::move(fallback));
}

std::string required_text(const Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> optional_integer(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != Kind::Literal) scenario_fail(std::string(name) + " must be a number");
    try {
        return std::stoll(value->text);
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " must be an integer");
    }
}

std::int64_t integer(const Value& object, std::string_view name, std::int64_t fallback) {
    return optional_integer(object, name).value_or(fallback);
}

int int32(const Value& object, std::string_view name, int fallback) {
    const std::optional<std::int64_t> value = optional_integer(object, name);
    return value.has_value() ? static_cast<int>(*value) : fallback;
}

bool boolean(const Value& object, std::string_view name, bool fallback) {
    const Value* value = field(object, name);
    if (is_null(value)) return fallback;
    return value->text == "true";
}

archive::RunStatus parse_status(std::string_view value) {
    if (value == "ACTIVE") return archive::RunStatus::Active;
    if (value == "STOPPING") return archive::RunStatus::Stopping;
    if (value == "COMPLETED") return archive::RunStatus::Completed;
    if (value == "STOPPED") return archive::RunStatus::Stopped;
    if (value == "FAILED") return archive::RunStatus::Failed;
    scenario_fail("unknown scenario RunStatus " + std::string(value));
}

archive::RunEndReason parse_end_reason(std::string_view value) {
    if (value == "MODEL_COMPLETED") return archive::RunEndReason::ModelCompleted;
    if (value == "USER_STOPPED") return archive::RunEndReason::UserStopped;
    if (value == "PROCESS_RECOVERED") return archive::RunEndReason::ProcessRecovered;
    if (value == "PROVIDER_ERROR") return archive::RunEndReason::ProviderError;
    scenario_fail("unknown scenario RunEndReason " + std::string(value));
}

std::string status_name(archive::RunStatus status) {
    switch (status) {
        case archive::RunStatus::Active: return "ACTIVE";
        case archive::RunStatus::Stopping: return "STOPPING";
        case archive::RunStatus::Completed: return "COMPLETED";
        case archive::RunStatus::Stopped: return "STOPPED";
        case archive::RunStatus::Failed: return "FAILED";
    }
    scenario_fail("unhandled RunStatus");
}

std::string reason_name(archive::RunEndReason reason) {
    switch (reason) {
        case archive::RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case archive::RunEndReason::UserStopped: return "USER_STOPPED";
        case archive::RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case archive::RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    scenario_fail("unhandled RunEndReason");
}

std::string encode_reason(const std::optional<archive::RunEndReason>& reason) {
    return reason.has_value() ? encode_text(reason_name(*reason)) : "n";
}

std::string encode_run(const archive::RunEntity& run) {
    std::string out = "id=" + encode_text(run.id);
    out += ";conv=" + encode_text(run.conversation_id);
    out += ";parent=" + encode_text(run.parent_run_id);
    out += ";status=" + encode_text(status_name(run.status));
    out += ";slot=" + encode_int(run.active_slot);
    out += ";started=" + encode_long(run.started_at);
    out += ";checkpoint=" + encode_long(run.last_checkpoint_at);
    out += ";stop=" + encode_long(run.stop_requested_at);
    out += ";ended=" + encode_long(run.ended_at);
    out += ";reason=" + encode_reason(run.end_reason);
    out += ";pass=" + encode_int(run.current_pass);
    out += ";ambiguous=" + encode_bool(run.legacy_ambiguous);
    return out;
}

std::string encode_snapshot(const archive::ArchivedRunSnapshot& snapshot) {
    std::string out = "id=" + encode_text(snapshot.id);
    out += ";conv=" + encode_text(snapshot.conversation_id);
    out += ";parent=" + encode_text(snapshot.parent_run_id);
    out += ";status=" + encode_text(snapshot.status);
    out += ";started=" + encode_long(snapshot.started_at);
    out += ";checkpoint=" + encode_long(snapshot.last_checkpoint_at);
    out += ";stop=" + encode_long(snapshot.stop_requested_at);
    out += ";ended=" + encode_long(snapshot.ended_at);
    out += ";reason=" + encode_text(snapshot.end_reason);
    out += ";pass=" + encode_int(snapshot.current_pass);
    out += ";ambiguous=" + encode_bool(snapshot.legacy_ambiguous);
    return out;
}

std::string encode_ownership(const archive::ArchivedMessageRunOwnership& item) {
    std::string out = "message=" + encode_text(item.message_id);
    out += ";conv=" + encode_text(item.conversation_id);
    out += ";run=" + encode_text(item.run_id);
    out += ";sequence=" + encode_long(item.run_sequence);
    out += ";consumed=" + encode_int(item.consumed_at_pass);
    return out;
}

std::string join(const std::vector<std::string>& parts) {
    std::string out;
    for (std::size_t i = 0; i < parts.size(); ++i) {
        if (i > 0) out.push_back('|');
        out += parts[i];
    }
    return out;
}

archive::ArchivedRunSnapshot parse_snapshot(const Value& root) {
    archive::ArchivedRunSnapshot snapshot;
    snapshot.id = required_text(root, "id");
    snapshot.conversation_id = text(root, "conversationId", "conv-a");
    snapshot.parent_run_id = optional_text(root, "parentRunId");
    snapshot.status = text(root, "status");
    snapshot.started_at = integer(root, "startedAt", 0);
    snapshot.last_checkpoint_at = integer(root, "lastCheckpointAt", 0);
    snapshot.stop_requested_at = optional_integer(root, "stopRequestedAt");
    snapshot.ended_at = optional_integer(root, "endedAt");
    snapshot.end_reason = optional_text(root, "endReason");
    snapshot.current_pass = int32(root, "currentPass", 0);
    snapshot.legacy_ambiguous = boolean(root, "legacyAmbiguous", false);
    return snapshot;
}

archive::RunEntity parse_run(const Value& object) {
    const archive::RunStatus status = parse_status(text(object, "status", "COMPLETED"));
    const std::int64_t started_at = integer(object, "startedAt", 1);
    const bool terminal = archive::is_terminal(status);

    archive::RunEntity run;
    run.id = required_text(object, "id");
    run.conversation_id = text(object, "conversationId", "conv-a");
    run.parent_run_id = optional_text(object, "parentRunId");
    run.status = status;
    run.active_slot = terminal ? std::nullopt : std::optional<int>(1);
    run.started_at = started_at;
    run.last_checkpoint_at = integer(object, "lastCheckpointAt", started_at);
    if (status == archive::RunStatus::Stopping) {
        run.stop_requested_at =
            optional_integer(object, "stopRequestedAt").value_or(started_at);
    } else {
        run.stop_requested_at = optional_integer(object, "stopRequestedAt");
    }
    run.ended_at =
        terminal
            ? std::optional<std::int64_t>(
                  optional_integer(object, "endedAt").value_or(started_at + 1))
            : std::nullopt;
    run.end_reason = terminal
                         ? std::optional<archive::RunEndReason>(
                               parse_end_reason(text(object, "endReason", "MODEL_COMPLETED")))
                         : std::nullopt;
    run.current_pass = int32(object, "currentPass", 0);
    run.legacy_ambiguous = boolean(object, "legacyAmbiguous", false);
    return run;
}

std::vector<archive::RunEntity> parse_runs(const Value& root) {
    const Value* value = field(root, "runs");
    std::vector<archive::RunEntity> runs;
    if (is_null(value)) return runs;
    if (value->kind != Kind::Array) scenario_fail("runs must be an array");
    runs.reserve(value->elements.size());
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail("a run must be an object");
        runs.push_back(parse_run(element));
    }
    return runs;
}

std::vector<archive::ArchivedMessageRunOwnership> parse_ownership(const Value& root) {
    const Value* value = field(root, "ownership");
    std::vector<archive::ArchivedMessageRunOwnership> ownership;
    if (is_null(value)) return ownership;
    if (value->kind != Kind::Array) scenario_fail("ownership must be an array");
    ownership.reserve(value->elements.size());
    for (const Value& element : value->elements) {
        if (element.kind != Kind::Object) scenario_fail("an ownership row must be an object");
        archive::ArchivedMessageRunOwnership item;
        item.message_id = required_text(element, "messageId");
        item.conversation_id = text(element, "conversationId", "conv-a");
        item.run_id = optional_text(element, "runId");
        item.run_sequence = optional_integer(element, "runSequence");
        item.consumed_at_pass =
            optional_integer(element, "consumedAtPass")
                .has_value()
                ? std::optional<int>(int32(element, "consumedAtPass", 0))
                : std::nullopt;
        ownership.push_back(std::move(item));
    }
    return ownership;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(stem + ": not an object");
    }
    const Value& root = *parsed.value;
    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "terminalize") {
        const archive::ArchivedRunSnapshot snapshot = parse_snapshot(root);
        trace += "snapshot|" + encode_snapshot(snapshot) + "\n";
        trace += "out|" + encode_run(archive::terminalize(snapshot)) + "\n";
    } else if (op == "hasCompleteOwnership") {
        const std::vector<archive::RunEntity> runs = parse_runs(root);
        const std::vector<archive::ArchivedMessageRunOwnership> ownership =
            parse_ownership(root);
        const bool unique = boolean(root, "sourceRunIdsWereUnique", true);

        std::vector<std::string> encoded_runs;
        encoded_runs.reserve(runs.size());
        for (const archive::RunEntity& run : runs) encoded_runs.push_back(encode_run(run));
        std::vector<std::string> encoded_ownership;
        encoded_ownership.reserve(ownership.size());
        for (const auto& item : ownership) {
            encoded_ownership.push_back(encode_ownership(item));
        }

        trace += "runs|" + join(encoded_runs) + "\n";
        trace += "ownership|" + join(encoded_ownership) + "\n";
        trace += "sourceRunIdsWereUnique|" + encode_bool(unique) + "\n";
        trace += "out|" + encode_bool(archive::has_complete_ownership(runs, ownership, unique)) +
                 "\n";
    } else if (op == "orderByParent") {
        const std::vector<archive::RunEntity> runs = parse_runs(root);
        const std::vector<archive::RunEntity> ordered = archive::order_by_parent(runs);
        std::vector<std::string> encoded_input;
        encoded_input.reserve(runs.size());
        for (const archive::RunEntity& run : runs) encoded_input.push_back(encode_run(run));
        std::vector<std::string> encoded_output;
        encoded_output.reserve(ordered.size());
        for (const archive::RunEntity& run : ordered) encoded_output.push_back(encode_run(run));

        trace += "input|" + join(encoded_input) + "\n";
        trace += "out|" + join(encoded_output) + "\n";
    } else {
        scenario_fail(stem + ": unknown op " + op);
    }
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 60 ? at - 60 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(240, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(native_run_archive_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_NATIVE_RUN_ARCHIVE_DIR;
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
        const std::string expected =
            read_file(golden_dir / (scenario.stem().string() + ".trace"));
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
