// Golden parity test for the legacy Run backfill planner.
//
// Replays every scenario under core/tools/golden/legacy-run-backfill/scenarios against the shipped
// port and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format mirrors the reference driver
// (core/tools/golden/legacy-run-backfill/LegacyRunBackfillTraceDump.kt).
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
#include "agora/core/legacy_run_backfill.hpp"
#include "check.hpp"

namespace {

namespace backfill = agora::core::legacy_run_backfill;
namespace json = agora::core::json;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

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
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::optional<std::int64_t> number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind != json::Kind::Literal) return std::nullopt;
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) return std::nullopt;
        return parsed;
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

bool literal_true(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "true";
}

std::vector<const json::Value*> objects(const json::Value& root, std::string_view name) {
    std::vector<const json::Value*> found;
    const json::Value* value = field(root, name);
    if (value == nullptr || value->kind != json::Kind::Array) return found;
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::Object) scenario_fail(std::string(name) + " item not object");
        found.push_back(&element);
    }
    return found;
}

using NullableSelections = backfill::NullableSelections;

NullableSelections selections(const json::Value& root, std::string_view name) {
    NullableSelections out;
    const json::Value* value = field(root, name);
    if (value == nullptr || value->kind != json::Kind::Object) return out;
    for (const json::Member& member : value->members) {
        if (member.value.kind != json::Kind::String) {
            scenario_fail(std::string(name) + " values must be strings");
        }
        const std::optional<std::string> key =
            member.key == "null" ? std::nullopt : std::optional<std::string>(member.key);
        bool updated = false;
        for (auto& entry : out) {
            if (entry.first == key) {
                entry.second = member.value.text;
                updated = true;
                break;
            }
        }
        if (!updated) out.emplace_back(key, member.value.text);
    }
    return out;
}

backfill::Participant participant_from_name(const std::string& name) {
    if (name == "USER") return backfill::Participant::User;
    if (name == "MODEL") return backfill::Participant::Model;
    if (name == "ERROR") return backfill::Participant::Error;
    scenario_fail("unknown participant " + name);
}

backfill::MessageStatus status_from_name(const std::string& name) {
    if (name == "TRANSCRIBING") return backfill::MessageStatus::Transcribing;
    if (name == "SENDING") return backfill::MessageStatus::Sending;
    if (name == "THINKING") return backfill::MessageStatus::Thinking;
    if (name == "TOOL_CALLING") return backfill::MessageStatus::ToolCalling;
    if (name == "SUCCESS") return backfill::MessageStatus::Success;
    if (name == "STOPPED") return backfill::MessageStatus::Stopped;
    if (name == "ERROR") return backfill::MessageStatus::Error;
    scenario_fail("unknown status " + name);
}

bool has_prefix(const std::string& text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

std::vector<backfill::LegacyMessageRecord> messages(const json::Value& root) {
    std::vector<backfill::LegacyMessageRecord> out;
    for (const json::Value* item : objects(root, "messages")) {
        backfill::LegacyMessageRecord message;
        message.id = required_text(*item, "id");
        message.parent_id = optional_text(*item, "parentId");
        message.participant = participant_from_name(required_text(*item, "participant"));
        message.status = status_from_name(required_text(*item, "status"));
        message.timestamp = number(*item, "timestamp").value_or(0);
        if (field(*item, "isSynthetic") != nullptr) {
            message.is_synthetic = literal_true(*item, "isSynthetic");
        } else {
            message.is_synthetic =
                has_prefix(message.id, "tool_") || has_prefix(message.id, "result_");
        }
        out.push_back(std::move(message));
    }
    return out;
}

std::string render_plan(const backfill::LegacyRunBackfillPlan& plan) {
    std::string out = "runs=";
    for (std::size_t index = 0; index < plan.runs.size(); ++index) {
        if (index != 0) out += ';';
        out += backfill::describe(plan.runs[index]);
    }
    out += "|assignments=";
    for (std::size_t index = 0; index < plan.assignments.size(); ++index) {
        if (index != 0) out += ';';
        out += backfill::describe(plan.assignments[index]);
    }
    return out;
}

std::string render_selections(const NullableSelections& selections) {
    std::string out;
    for (std::size_t index = 0; index < selections.size(); ++index) {
        if (index != 0) out += ';';
        out += selections[index].first.has_value() ? *selections[index].first : "<null>";
        out += '=';
        out += selections[index].second;
    }
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem + ": not an object");
    }
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = optional_text(root, "op").value_or("");
    if (op.empty()) scenario_fail(stem + ": op is required");
    const std::string conversation_id = optional_text(root, "conversationId").value_or("");
    const std::vector<backfill::LegacyMessageRecord> records = messages(root);

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    trace += "args|" + esc(conversation_id) + "|" + std::to_string(records.size()) + "\n";

    const backfill::PlanResult plan = backfill::plan(conversation_id, records);
    if (op == "plan") {
        if (!plan.ok) {
            trace += "result|ERROR|" + esc(plan.error) + "\n";
        } else {
            trace += "result|" + esc(render_plan(plan.plan)) + "\n";
        }
    } else if (op == "selected") {
        if (!plan.ok) {
            trace += "result|ERROR|" + esc(plan.error) + "\n";
        } else {
            const backfill::SelectionResult selected = backfill::selected_run_branches(
                records, plan.plan, selections(root, "selectedMessageBranches"));
            if (!selected.ok) {
                trace += "result|ERROR|" + esc(selected.error) + "\n";
            } else {
                trace += "result|" + esc(render_selections(selected.selections)) + "\n";
            }
        }
    } else {
        scenario_fail(stem + ": unknown op " + op);
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

TEST(legacy_run_backfill_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_LEGACY_RUN_BACKFILL_DIR;
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
