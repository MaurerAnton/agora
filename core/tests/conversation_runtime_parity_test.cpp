// Golden parity test for the native conversation runtime reducer.
//
// Replays every scenario under core/tools/golden/conversation-runtime/scenarios through
// agora::core::conversation_runtime and requires the produced trace to equal the frozen Kotlin
// trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/conversation-runtime/ConversationRuntimeTraceDump.kt): it prints the command,
// the resulting state, the emitted effects and the rejection of every step, all through the
// Kotlin data-class `toString()` shapes that `describe(...)` reproduces.
//
// The pins that matter: every declared field of every state/effect/command, in declaration order,
// including nested identities and phase objects; the identity algebra (effect identities derive
// from run identities, `derived("tool-batch")` prefixes an effect id); and the full rejection
// surface (`ILLEGAL_STATE` / `STALE_IDENTITY` / `DUPLICATE_RESULT`), which is what distinguishes a
// reducer port that merely runs from one that orders its checks the way the app does.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conversation_runtime.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace cr = agora::core::conversation_runtime;
using json::Kind;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** The reference driver's esc: backslash, separators and control characters only. */
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

const json::Value& require_object(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Object) {
        scenario_fail(std::string(name) + " must be an object");
    }
    return *value;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == Kind::Literal && value->text == "null");
}

/** nullopt for an absent or JSON-null field, otherwise the string -- empty is a real value. */
std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

/** Kotlin `longOrNull`: numbers are unquoted runs, but a numeric string would decode too. */
std::optional<std::int64_t> number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind == Kind::Array || value->kind == Kind::Object) {
        return std::nullopt;
    }
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool flag(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return false;
    return value->kind == Kind::Literal && value->text == "true";
}

cr::RunStatus run_status_of(std::string_view name) {
    if (name == "ACTIVE") return cr::RunStatus::Active;
    if (name == "STOPPING") return cr::RunStatus::Stopping;
    if (name == "COMPLETED") return cr::RunStatus::Completed;
    if (name == "STOPPED") return cr::RunStatus::Stopped;
    if (name == "FAILED") return cr::RunStatus::Failed;
    scenario_fail("unknown RunStatus: " + std::string(name));
}

cr::RunEndReason end_reason_of(std::string_view name) {
    if (name == "MODEL_COMPLETED") return cr::RunEndReason::ModelCompleted;
    if (name == "USER_STOPPED") return cr::RunEndReason::UserStopped;
    if (name == "PROCESS_RECOVERED") return cr::RunEndReason::ProcessRecovered;
    if (name == "PROVIDER_ERROR") return cr::RunEndReason::ProviderError;
    scenario_fail("unknown RunEndReason: " + std::string(name));
}

cr::ProviderPassResult pass_result_of(std::string_view name) {
    if (name == "COMPLETED_TEXT") return cr::ProviderPassResult::CompletedText;
    if (name == "COMPLETED_TOOL_CALLS") return cr::ProviderPassResult::CompletedToolCalls;
    if (name == "TRUNCATED") return cr::ProviderPassResult::Truncated;
    if (name == "FAILED") return cr::ProviderPassResult::Failed;
    if (name == "CANCELLED") return cr::ProviderPassResult::Cancelled;
    scenario_fail("unknown ProviderPassResult: " + std::string(name));
}

/** The reference driver's Builder: identity defaults are part of the pinned scenario schema. */
class Builder {
public:
    explicit Builder(std::string conversation_id) : conversation_id_(std::move(conversation_id)) {}

    cr::RunEffectIdentity effect_identity(const json::Value& value,
                                          std::string_view default_run_id = "run",
                                          std::string_view default_effect_id = "e") const {
        cr::RunEffectIdentity identity;
        identity.conversation_id =
            optional_text(value, "conversationId").value_or(conversation_id_);
        identity.owner_token = number(value, "ownerToken").value_or(1);
        identity.run_id = optional_text(value, "runId").value_or(std::string(default_run_id));
        identity.pass = static_cast<int>(number(value, "pass").value_or(0));
        identity.effect_id =
            optional_text(value, "effectId").value_or(std::string(default_effect_id));
        return identity;
    }

    cr::RuntimeRunIdentity runtime_identity(const json::Value& value) const {
        cr::RuntimeRunIdentity identity;
        identity.conversation_id =
            optional_text(value, "conversationId").value_or(conversation_id_);
        identity.owner_token = number(value, "ownerToken").value_or(1);
        identity.run_id = optional_text(value, "runId");
        identity.pass = static_cast<int>(number(value, "pass").value_or(0));
        return identity;
    }

    cr::Command command(const json::Value& value) const {
        const std::string kind = required_text(value, "kind");
        cr::Command out;
        if (kind == "Recover") {
            const json::Value& snapshot = require_object(value, "snapshot");
            out.kind = cr::Command::Kind::Recover;
            out.snapshot.conversation_id =
                optional_text(snapshot, "conversationId").value_or(conversation_id_);
            out.snapshot.run_id = required_text(snapshot, "runId");
            out.snapshot.pass = static_cast<int>(number(snapshot, "pass").value_or(0));
            out.snapshot.status = run_status_of(required_text(snapshot, "status"));
            return out;
        }
        if (kind == "RecoveryCompleted") {
            out.kind = cr::Command::Kind::RecoveryCompleted;
            out.identity = effect_identity(require_object(value, "identity"));
            out.success = flag(value, "success");
            return out;
        }
        if (kind == "AcquireSlot") {
            out.kind = cr::Command::Kind::AcquireSlot;
            out.runtime_identity = runtime_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "SendRequested") {
            out.kind = cr::Command::Kind::SendRequested;
            out.identity = effect_identity(require_object(value, "identity"));
            out.direct_only = flag(value, "directOnly");
            out.has_pending_guidance = flag(value, "hasPendingGuidance");
            return out;
        }
        if (kind == "InputPersisted") {
            out.kind = cr::Command::Kind::InputPersisted;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "InputPersistenceFailed") {
            out.kind = cr::Command::Kind::InputPersistenceFailed;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "SendLaunchAbandoned") {
            out.kind = cr::Command::Kind::SendLaunchAbandoned;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "BindRun") {
            out.kind = cr::Command::Kind::BindRun;
            out.runtime_identity = runtime_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "ProviderPassRequested") {
            out.kind = cr::Command::Kind::ProviderPassRequested;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "ProviderPassCompleted") {
            out.kind = cr::Command::Kind::ProviderPassCompleted;
            out.identity = effect_identity(require_object(value, "identity"));
            out.result = pass_result_of(required_text(value, "result"));
            return out;
        }
        if (kind == "ToolBatchRequested") {
            out.kind = cr::Command::Kind::ToolBatchRequested;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "ToolBatchCompleted") {
            out.kind = cr::Command::Kind::ToolBatchCompleted;
            out.identity = effect_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "ToolRoundCommitted") {
            out.kind = cr::Command::Kind::ToolRoundCommitted;
            out.identity = effect_identity(require_object(value, "identity"));
            out.success = flag(value, "success");
            return out;
        }
        if (kind == "FinalizationRequested") {
            out.kind = cr::Command::Kind::FinalizationRequested;
            out.identity = effect_identity(require_object(value, "identity"));
            out.status = run_status_of(required_text(value, "status"));
            out.reason = end_reason_of(required_text(value, "reason"));
            out.mark_conversation_unread = flag(value, "markConversationUnread");
            return out;
        }
        if (kind == "FinalizationCompleted") {
            out.kind = cr::Command::Kind::FinalizationCompleted;
            out.identity = effect_identity(require_object(value, "identity"));
            out.success = flag(value, "success");
            return out;
        }
        if (kind == "StopRequested") {
            out.kind = cr::Command::Kind::StopRequested;
            out.runtime_identity = runtime_identity(require_object(value, "identity"));
            out.coroutine_already_settled = flag(value, "coroutineAlreadySettled");
            out.requires_persistence = flag(value, "requiresPersistence");
            out.effect_id = optional_text(value, "effectId");
            return out;
        }
        if (kind == "CoroutineSettled") {
            out.kind = cr::Command::Kind::CoroutineSettled;
            out.runtime_identity = runtime_identity(require_object(value, "identity"));
            return out;
        }
        if (kind == "PersistenceSettled") {
            out.kind = cr::Command::Kind::PersistenceSettled;
            out.identity = effect_identity(require_object(value, "identity"));
            out.success = flag(value, "success");
            return out;
        }
        scenario_fail("unknown command kind: " + kind);
    }

private:
    std::string conversation_id_;
};

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) scenario_fail(stem + ": not an object");
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string conversation_id = optional_text(root, "conversationId").value_or("c");
    const Builder builder(conversation_id);

    cr::RunState state;
    state.kind = cr::RunState::Kind::Idle;
    state.conversation_id = conversation_id;

    std::string trace = "name|" + esc(name) + "\n";
    trace += "initial|" + esc(cr::describe(state)) + "\n";

    const json::Value* commands = field(root, "commands");
    if (commands == nullptr || commands->kind != Kind::Array) {
        scenario_fail(stem + ": commands must be an array");
    }
    for (std::size_t index = 0; index < commands->elements.size(); ++index) {
        const cr::Command command = builder.command(commands->elements[index]);
        const cr::Transition transition = cr::reduce(state, command);
        const std::string position = std::to_string(index);
        trace += "cmd|" + position + "|" + esc(cr::describe(command)) + "\n";
        trace += "state|" + position + "|" + esc(cr::describe(transition.state)) + "\n";
        trace += "effects|" + position + "|" + esc(cr::describe(transition.effects)) + "\n";
        trace += "reject|" + position + "|";
        trace += transition.rejection.has_value() ? cr::describe(*transition.rejection) : "-";
        trace += "\n";
        state = transition.state;
    }
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(200, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(conversation_runtime_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CONVERSATION_RUNTIME_DIR;
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
        const std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
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
