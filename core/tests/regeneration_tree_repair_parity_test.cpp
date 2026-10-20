// Golden parity test for the regeneration-tree repair planner.
//
// Replays every scenario under core/tools/golden/regeneration-tree-repair/scenarios against the
// shipped port and requires the produced trace to equal the frozen Kotlin trace byte for byte.
// The trace format mirrors the reference driver
// (core/tools/golden/regeneration-tree-repair/RegenerationTreeRepairTraceDump.kt).
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
#include "agora/core/regeneration_tree_repair.hpp"
#include "check.hpp"

namespace {

namespace repair = agora::core::regeneration_tree_repair;
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

/** Kotlin Map<String?, String>; the key `null` is the nullable null key, duplicates last-wins. */
using NullableKeyMap = std::vector<std::pair<std::optional<std::string>, std::string>>;

NullableKeyMap selections(const json::Value& root, std::string_view name) {
    NullableKeyMap out;
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

repair::Participant participant_from_name(const std::string& name) {
    if (name == "USER") return repair::Participant::User;
    if (name == "MODEL") return repair::Participant::Model;
    if (name == "ERROR") return repair::Participant::Error;
    scenario_fail("unknown participant " + name);
}

std::string pairs(const std::vector<std::pair<std::string, std::string>>& entries) {
    std::string out;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out += ';';
        out += entries[index].first;
        out += '=';
        out += entries[index].second;
    }
    return out;
}

std::string sequences(const std::vector<std::pair<std::string, long long>>& entries) {
    std::string out;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out += ';';
        out += entries[index].first;
        out += '=';
        out += std::to_string(entries[index].second);
    }
    return out;
}

std::string nullable_pairs(const NullableKeyMap& entries) {
    std::string out;
    for (std::size_t index = 0; index < entries.size(); ++index) {
        if (index != 0) out += ';';
        out += entries[index].first.has_value() ? *entries[index].first : "<null>";
        out += '=';
        out += entries[index].second;
    }
    return out;
}

std::string joined(const std::vector<std::string>& ids) {
    std::string out;
    for (std::size_t index = 0; index < ids.size(); ++index) {
        if (index != 0) out += ',';
        out += ids[index];
    }
    return out;
}

std::string render(const repair::Plan& plan) {
    return "runParent=" + pairs(plan.run_parent_updates) +
           "|deleted=" + joined(plan.deleted_message_ids) +
           "|messageParent=" + pairs(plan.message_parent_updates) +
           "|runSequence=" + sequences(plan.run_sequence_updates) +
           "|messageSelections=" + nullable_pairs(plan.message_selections) +
           "|runSelections=" + nullable_pairs(plan.run_selections) +
           "|inferred=" + joined(plan.inferred_run_ids);
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

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "plan") {
        std::vector<repair::V17RunRecord> runs;
        for (const json::Value* item : objects(root, "runs")) {
            repair::V17RunRecord run;
            run.id = required_text(*item, "id");
            run.parent_run_id = optional_text(*item, "parentRunId");
            run.started_at = number(*item, "startedAt").value_or(0);
            runs.push_back(std::move(run));
        }
        std::vector<repair::V17MessageRecord> messages;
        for (const json::Value* item : objects(root, "messages")) {
            repair::V17MessageRecord message;
            message.id = required_text(*item, "id");
            message.parent_id = optional_text(*item, "parentId");
            message.participant = participant_from_name(required_text(*item, "participant"));
            message.timestamp = number(*item, "timestamp").value_or(0);
            message.run_id = required_text(*item, "runId");
            message.run_sequence = number(*item, "runSequence").value_or(0);
            message.input_fingerprint = required_text(*item, "inputFingerprint");
            messages.push_back(std::move(message));
        }

        const repair::Plan plan = repair::plan(runs, messages, selections(root, "messageSelections"),
                                               selections(root, "runSelections"));
        trace += "args|" + std::to_string(runs.size()) + "|" + std::to_string(messages.size()) + "\n";
        trace += "result|" + esc(render(plan)) + "\n";
    } else if (op == "fingerprint") {
        const std::string text = optional_text(root, "text").value_or("");
        const int image_count = static_cast<int>(number(root, "imageCount").value_or(0));
        const std::optional<std::string> attachment_meta = optional_text(root, "attachmentMeta");
        trace += "args|" + esc(text) + "|" + std::to_string(image_count) + "|" +
                 (attachment_meta.has_value() ? esc(*attachment_meta) : "-") + "\n";
        trace += "result|" +
                 esc(repair::regeneration_input_fingerprint(text, image_count, attachment_meta)) +
                 "\n";
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

TEST(regeneration_tree_repair_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_REGENERATION_TREE_REPAIR_DIR;
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
