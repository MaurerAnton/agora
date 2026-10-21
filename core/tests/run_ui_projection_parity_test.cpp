// Golden parity test for the Run UI projection (message generation boundaries and affordances).
//
// The scenario supplies the visible messages and the full message list; the port must reproduce
// the resolved boundaries and every per-message presentation field.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/run_ui_projection.hpp"
#include "check.hpp"

namespace {

namespace run_ui = agora::core::run_ui;
namespace prep = agora::core::prep;
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
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

prep::ChatMessage build_message(const Value& spec) {
    prep::ChatMessage message;
    message.id = required_string(spec, "id");
    message.parent_id = optional_string(spec, "parentId");
    message.text = optional_string(spec, "text").value_or("");
    message.run_id = optional_string(spec, "runId");
    message.timestamp = field(spec, "timestamp") != nullptr
                            ? std::stoll(field(spec, "timestamp")->text)
                            : 0;
    const std::string participant = optional_string(spec, "participant").value_or("USER");
    if (participant == "MODEL") {
        message.participant = prep::Participant::Model;
    } else if (participant == "ERROR") {
        message.participant = prep::Participant::Error;
    } else {
        message.participant = prep::Participant::User;
    }
    const std::string status = optional_string(spec, "status").value_or("SUCCESS");
    if (status == "ERROR") {
        message.status = prep::MessageStatus::Error;
    } else if (status == "STOPPED") {
        message.status = prep::MessageStatus::Stopped;
    } else if (status == "THINKING") {
        message.status = prep::MessageStatus::Thinking;
    } else if (status == "SENDING") {
        message.status = prep::MessageStatus::Sending;
    } else if (status == "TOOL_CALLING") {
        message.status = prep::MessageStatus::ToolCalling;
    } else if (status == "TRANSCRIBING") {
        message.status = prep::MessageStatus::Transcribing;
    } else {
        message.status = prep::MessageStatus::Success;
    }
    return message;
}

std::vector<prep::ChatMessage> build_messages(const Value& root, std::string_view name) {
    std::vector<prep::ChatMessage> messages;
    const Value* list = field(root, name);
    if (list == nullptr) return messages;
    for (const Value& element : list->elements) messages.push_back(build_message(element));
    return messages;
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

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::vector<prep::ChatMessage> visible = build_messages(root, "visibleMessages");
    const std::vector<prep::ChatMessage> all = build_messages(root, "allMessages");

    const std::vector<run_ui::GenerationBoundary> boundaries = run_ui::resolve_boundaries(visible);
    const std::vector<std::pair<std::string, run_ui::MessagePresentation>> projection =
        run_ui::project(visible, all);

    std::string trace = "{\"boundaries\":[";
    for (std::size_t index = 0; index < boundaries.size(); ++index) {
        if (index != 0) trace.push_back(',');
        const run_ui::GenerationBoundary& boundary = boundaries[index];
        trace += "{\"messages\":[";
        for (std::size_t position = 0; position < boundary.messages.size(); ++position) {
            if (position != 0) trace.push_back(',');
            append_quoted(trace, boundary.messages[position].id);
        }
        trace += "],\"input\":";
        append_optional(trace, boundary.input != nullptr
                                    ? std::optional<std::string>(boundary.input->id)
                                    : std::nullopt);
        trace += ",\"firstAssistant\":";
        append_optional(trace, boundary.first_assistant != nullptr
                                    ? std::optional<std::string>(boundary.first_assistant->id)
                                    : std::nullopt);
        trace += ",\"lastAssistant\":";
        append_optional(trace, boundary.last_assistant != nullptr
                                    ? std::optional<std::string>(boundary.last_assistant->id)
                                    : std::nullopt);
        trace.push_back('}');
    }
    trace += "],\"presentation\":{";
    for (std::size_t index = 0; index < projection.size(); ++index) {
        if (index != 0) trace.push_back(',');
        const auto& entry = projection[index];
        trace += "\"";
        json::escape_to(trace, entry.first);
        trace += "\":{\"showActions\":";
        trace += entry.second.show_actions ? "true" : "false";
        trace += ",\"copyText\":";
        append_optional(trace, entry.second.copy_text);
        trace += ",\"deleteTargetMessageId\":";
        append_optional(trace, entry.second.delete_target_message_id);
        trace += ",\"showBranchSelector\":";
        trace += entry.second.show_branch_selector ? "true" : "false";
        trace += ",\"branchIndex\":" + std::to_string(entry.second.branch_index);
        trace += ",\"totalBranches\":" + std::to_string(entry.second.total_branches);
        trace += ",\"branchAnchorParentId\":";
        append_optional(trace, entry.second.branch_anchor_parent_id);
        trace += ",\"branchAnchorMessageId\":";
        append_optional(trace, entry.second.branch_anchor_message_id);
        trace.push_back('}');
    }
    trace += "}}";
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

TEST(run_ui_projection_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_RUN_UI_PROJECTION_DIR;
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
