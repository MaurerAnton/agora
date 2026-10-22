// Golden parity test for the generation policies (terminal disposition, throttle gates, thought
// segments, message templates and the API-only history projection).
//
// The scenario runs the same policies the Kotlin driver runs; the trace pins every branch.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/generation_policies.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace policy = agora::core::generation_policy;
namespace prep = agora::core::prep;
namespace source_model = agora::core::message_source;
namespace runtime = agora::core::conversation_runtime;
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

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::vector<std::string> string_list(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return {};
    if (value->kind != Kind::Array) scenario_fail(std::string(name) + " must be an array");
    std::vector<std::string> out;
    for (const Value& element : value->elements) {
        if (element.kind != Kind::String) scenario_fail(std::string(name) + " must be strings");
        out.push_back(element.text);
    }
    return out;
}

prep::MessageStatus parse_status(const std::string& name) {
    if (name == "ERROR") return prep::MessageStatus::Error;
    if (name == "STOPPED") return prep::MessageStatus::Stopped;
    if (name == "THINKING") return prep::MessageStatus::Thinking;
    if (name == "SENDING") return prep::MessageStatus::Sending;
    if (name == "TOOL_CALLING") return prep::MessageStatus::ToolCalling;
    if (name == "TRANSCRIBING") return prep::MessageStatus::Transcribing;
    return prep::MessageStatus::Success;
}

prep::Participant parse_participant(const std::string& name) {
    if (name == "MODEL") return prep::Participant::Model;
    if (name == "ERROR") return prep::Participant::Error;
    return prep::Participant::User;
}

prep::MessageSegment build_segment(const Value& spec) {
    prep::MessageSegment segment;
    segment.type = required_string(spec, "type");
    segment.content = optional_string(spec, "content").value_or("");
    segment.signature = optional_string(spec, "signature");
    segment.signature_provider = optional_string(spec, "signatureProvider");
    if (const Value* duration = field(spec, "durationMs"); duration != nullptr) {
        segment.duration_ms = std::stoll(duration->text);
    }
    segment.error_code = optional_string(spec, "errorCode");
    return segment;
}

source_model::Source build_source(const Value& spec) {
    source_model::Source source;
    const std::string kind = optional_string(spec, "kind").value_or("task");
    if (kind == "loop") {
        source.kind = source_model::Kind::Loop;
    } else if (kind == "ask_user") {
        source.kind = source_model::Kind::AskUser;
    } else {
        source.kind = source_model::Kind::Task;
    }
    if (const Value* items = field(spec, "askUser"); items != nullptr &&
        items->kind == Kind::Array) {
        for (const Value& element : items->elements) {
            source_model::AskUserItem item;
            item.question = required_string(element, "question");
            item.answer = optional_string(element, "answer");
            source.ask_user.push_back(item);
        }
    }
    return source;
}

prep::ChatMessage build_message(const Value& spec) {
    prep::ChatMessage message;
    message.id = required_string(spec, "id");
    message.parent_id = optional_string(spec, "parentId");
    message.text = optional_string(spec, "text").value_or("");
    message.images = string_list(spec, "images");
    message.status = parse_status(optional_string(spec, "status").value_or("SUCCESS"));
    message.participant = parse_participant(optional_string(spec, "participant").value_or("USER"));
    message.timestamp = field(spec, "timestamp") != nullptr
                            ? std::stoll(field(spec, "timestamp")->text)
                            : 0;
    message.model_name = optional_string(spec, "modelName");
    if (const Value* segments = field(spec, "segments");
        segments != nullptr && segments->kind == Kind::Array) {
        std::vector<prep::MessageSegment> parsed;
        for (const Value& element : segments->elements) {
            parsed.push_back(build_segment(element));
        }
        message.segments = std::move(parsed);
    }
    message.run_id = optional_string(spec, "runId");
    if (const Value* source = field(spec, "source"); source != nullptr) {
        message.source = build_source(*source);
    }
    return message;
}

std::string status_name(prep::MessageStatus status) {
    switch (status) {
        case prep::MessageStatus::Transcribing: return "TRANSCRIBING";
        case prep::MessageStatus::Sending: return "SENDING";
        case prep::MessageStatus::Thinking: return "THINKING";
        case prep::MessageStatus::ToolCalling: return "TOOL_CALLING";
        case prep::MessageStatus::Success: return "SUCCESS";
        case prep::MessageStatus::Stopped: return "STOPPED";
        case prep::MessageStatus::Error: return "ERROR";
    }
    return "SUCCESS";
}

std::string participant_name(prep::Participant participant) {
    switch (participant) {
        case prep::Participant::User: return "USER";
        case prep::Participant::Model: return "MODEL";
        case prep::Participant::Error: return "ERROR";
    }
    return "USER";
}

std::string run_status_name(runtime::RunStatus status) {
    switch (status) {
        case runtime::RunStatus::Active: return "ACTIVE";
        case runtime::RunStatus::Stopping: return "STOPPING";
        case runtime::RunStatus::Completed: return "COMPLETED";
        case runtime::RunStatus::Stopped: return "STOPPED";
        case runtime::RunStatus::Failed: return "FAILED";
    }
    return "COMPLETED";
}

std::string run_end_reason_name(runtime::RunEndReason reason) {
    switch (reason) {
        case runtime::RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case runtime::RunEndReason::UserStopped: return "USER_STOPPED";
        case runtime::RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case runtime::RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    return "MODEL_COMPLETED";
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

void append_optional(std::string& out, const std::optional<std::string>& value) {
    if (value.has_value()) {
        append_quoted(out, *value);
    } else {
        out += "null";
    }
}

std::string render_message(const prep::ChatMessage& message) {
    std::string out = "{\"id\":";
    append_quoted(out, message.id);
    out += ",\"parentId\":";
    append_optional(out, message.parent_id);
    out += ",\"text\":";
    append_quoted(out, message.text);
    out += ",\"images\":[";
    for (std::size_t i = 0; i < message.images.size(); ++i) {
        if (i != 0) out.push_back(',');
        append_quoted(out, message.images[i]);
    }
    out += "],\"status\":";
    append_quoted(out, status_name(message.status));
    out += ",\"participant\":";
    append_quoted(out, participant_name(message.participant));
    out += ",\"timestamp\":" + std::to_string(message.timestamp);
    out += ",\"modelName\":";
    append_optional(out, message.model_name);
    out += ",\"segments\":[";
    if (message.segments.has_value()) {
        for (std::size_t i = 0; i < message.segments->size(); ++i) {
            if (i != 0) out.push_back(',');
            const prep::MessageSegment& segment = (*message.segments)[i];
            out += "{\"type\":";
            append_quoted(out, segment.type);
            out += ",\"content\":";
            append_quoted(out, segment.content);
            out += ",\"signature\":";
            append_optional(out, segment.signature);
            out += ",\"signatureProvider\":";
            append_optional(out, segment.signature_provider);
            out += ",\"durationMs\":";
            if (segment.duration_ms.has_value()) {
                out += std::to_string(*segment.duration_ms);
            } else {
                out += "null";
            }
            out += ",\"errorCode\":";
            append_optional(out, segment.error_code);
            out.push_back('}');
        }
    }
    out += "]}";
    return out;
}

std::string render_messages(const std::vector<prep::ChatMessage>& messages) {
    std::string out = "[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += render_message(messages[i]);
    }
    out.push_back(']');
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    std::vector<prep::ChatMessage> messages;
    if (const Value* list = field(root, "messages"); list != nullptr) {
        for (const Value& element : list->elements) messages.push_back(build_message(element));
    }

    std::string trace = "{\"terminalNotifications\":[";
    {
        bool first = true;
        for (prep::MessageStatus status : {prep::MessageStatus::Success, prep::MessageStatus::Error,
                                          prep::MessageStatus::Stopped,
                                          prep::MessageStatus::Thinking}) {
            for (bool pending : {false, true}) {
                for (bool compact : {false, true}) {
                    for (bool foreground : {false, true}) {
                        for (const std::optional<bool> visible :
                             {std::optional<bool>(std::nullopt), std::optional<bool>(false),
                              std::optional<bool>(true)}) {
                            if (!first) trace.push_back(',');
                            first = false;
                            trace += "{\"status\":";
                            append_quoted(trace, status_name(status));
                            trace += ",\"pending\":" + std::string(pending ? "true" : "false");
                            trace += ",\"compact\":" + std::string(compact ? "true" : "false");
                            trace += ",\"foreground\":" + std::string(foreground ? "true" : "false");
                            trace += ",\"visible\":";
                            if (visible.has_value()) {
                                trace += *visible ? "true" : "false";
                            } else {
                                trace += "null";
                            }
                            trace += ",\"notify\":";
                            trace += policy::should_post_generation_terminal_notification(
                                         status, pending, compact, foreground, visible)
                                         ? "true"
                                         : "false";
                            trace.push_back('}');
                        }
                    }
                }
            }
        }
    }
    trace += "],\"terminalDispositions\":[";
    {
        bool first = true;
        for (prep::MessageStatus status : {prep::MessageStatus::Success, prep::MessageStatus::Error,
                                          prep::MessageStatus::Stopped,
                                          prep::MessageStatus::Thinking}) {
            for (bool pending : {false, true}) {
                for (const std::optional<bool> visible :
                     {std::optional<bool>(std::nullopt), std::optional<bool>(false),
                      std::optional<bool>(true)}) {
                    if (!first) trace.push_back(',');
                    first = false;
                    const policy::TerminalDisposition disposition =
                        policy::generation_terminal_disposition(status, pending, visible);
                    trace += "{\"status\":";
                    append_quoted(trace, status_name(status));
                    trace += ",\"pending\":" + std::string(pending ? "true" : "false");
                    trace += ",\"visible\":";
                    if (visible.has_value()) {
                        trace += *visible ? "true" : "false";
                    } else {
                        trace += "null";
                    }
                    trace += ",\"runStatus\":";
                    append_quoted(trace, run_status_name(disposition.run_status));
                    trace += ",\"endReason\":";
                    append_quoted(trace, run_end_reason_name(disposition.end_reason));
                    trace += ",\"markUnread\":";
                    trace += disposition.mark_conversation_unread ? "true" : "false";
                    trace.push_back('}');
                }
            }
        }
    }
    trace += "],\"checkpointGate\":[";
    {
        policy::StreamingCheckpointGate gate(1'000);
        bool first = true;
        for (std::int64_t now : {0LL, 500LL, 999LL, 1000LL, 1500LL, 2500LL, 100LL}) {
            if (!first) trace.push_back(',');
            first = false;
            trace += "{\"now\":" + std::to_string(now) + ",\"checkpoint\":";
            trace += gate.should_checkpoint(now) ? "true" : "false";
            trace.push_back('}');
        }
    }
    trace += "],\"uiGate\":[";
    {
        policy::StreamingUiUpdateGate gate(50);
        bool first = true;
        for (std::int64_t now : {0LL, 10LL, 49LL, 50LL, 51LL, 5LL}) {
            if (!first) trace.push_back(',');
            first = false;
            const bool due = gate.is_due(now);
            if (due) gate.record_published(now);
            trace += "{\"now\":" + std::to_string(now) + ",\"due\":";
            trace += due ? "true" : "false";
            trace.push_back('}');
        }
    }
    trace += "],\"thoughtSegments\":[";
    {
        std::vector<prep::MessageSegment> segments;
        if (const Value* list = field(root, "segments"); list != nullptr) {
            for (const Value& element : list->elements) segments.push_back(build_segment(element));
        }
        bool first = true;
        for (int from : {-1, 0, 1, 2, 99}) {
            if (!first) trace.push_back(',');
            first = false;
            const std::vector<prep::MessageSegment> selected =
                policy::tool_round_thought_segments(segments, from);
            trace += "[";
            for (std::size_t i = 0; i < selected.size(); ++i) {
                if (i != 0) trace.push_back(',');
                trace += "{\"type\":";
                append_quoted(trace, selected[i].type);
                trace += ",\"content\":";
                append_quoted(trace, selected[i].content);
                trace.push_back('}');
            }
            trace.push_back(']');
        }
    }
    trace += "],\"compacted\":[";
    {
        policy::ToolRoundHistoryCompactor compactor;
        bool first = true;
        if (const Value* rounds = field(root, "compactorRounds");
            rounds != nullptr && rounds->kind == Kind::Array) {
            for (const Value& round : rounds->elements) {
                const std::string run_id = required_string(round, "runId");
                std::vector<prep::MessageSegment> segments;
                if (const Value* list = field(round, "segments");
                    list != nullptr && list->kind == Kind::Array) {
                    for (const Value& element : list->elements) {
                        segments.push_back(build_segment(element));
                    }
                }
                if (!first) trace.push_back(',');
                first = false;
                const std::vector<prep::MessageSegment> compacted =
                    compactor.compact(run_id, segments);
                trace += "[";
                for (std::size_t i = 0; i < compacted.size(); ++i) {
                    if (i != 0) trace.push_back(',');
                    trace += "{\"type\":";
                    append_quoted(trace, compacted[i].type);
                    trace += ",\"content\":";
                    append_quoted(trace, compacted[i].content);
                    trace.push_back('}');
                }
                trace.push_back(']');
            }
        }
    }
    trace += "]";

    std::optional<std::string> user_prepend;
    std::optional<std::string> user_postpend;
    std::optional<std::string> assistant_prepend;
    std::optional<std::string> assistant_postpend;
    if (const Value* templates = field(root, "templates"); templates != nullptr) {
        user_prepend = optional_string(*templates, "userPrepend");
        user_postpend = optional_string(*templates, "userPostpend");
        assistant_prepend = optional_string(*templates, "assistantPrepend");
        assistant_postpend = optional_string(*templates, "assistantPostpend");
    }

    trace += ",\"templated\":" +
             render_messages(policy::apply_message_templates_to_messages(
                 messages, user_prepend, user_postpend, assistant_prepend, assistant_postpend));
    trace += ",\"userTemplated\":" +
             render_messages(policy::apply_user_template_to_messages(messages, user_prepend,
                                                                     user_postpend));
    trace += ",\"automatic\":" +
             render_messages(policy::project_automatic_messages_for_api(messages));
    if (const Value* source_spec = field(root, "automaticSource"); source_spec != nullptr) {
        trace += ",\"automaticText\":";
        append_quoted(trace, policy::automatic_message_api_text(
                                 build_source(*source_spec),
                                 optional_string(*source_spec, "text").value_or("")));
    }
    if (const Value* projection = field(root, "projection"); projection != nullptr) {
        const Value* include = field(*projection, "includeImages");
        const bool include_images =
            include != nullptr && include->kind == Kind::Literal && include->text == "true";
        const std::optional<std::string> initial_prompt =
            optional_string(*projection, "initialUserPrompt");
        trace += ",\"projected\":" +
                 render_messages(policy::project_generation_input_messages(
                     messages, include_images, user_prepend, user_postpend, assistant_prepend,
                     assistant_postpend, initial_prompt));
    }
    trace.push_back('}');
    return trace;
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

TEST(generation_policies_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_GENERATION_POLICIES_DIR;
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
