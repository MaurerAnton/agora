// Golden parity test for the message-source codec.
//
// Replays every scenario under core/tools/golden/message-source/scenarios against the shipped
// port and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format mirrors the reference driver
// (core/tools/golden/message-source/MessageSourceTraceDump.kt): decode prints the data-class
// toString(), encode/sanitize the canonical stored JSON, readable the ask_user text form.
#include <algorithm>
#include <cstddef>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/message_source.hpp"
#include "check.hpp"

namespace {

namespace source = agora::core::message_source;
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

std::string opt(const std::optional<std::string>& value) {
    return value.has_value() ? esc(*value) : "-";
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

source::Participant participant_from_name(const std::string& name) {
    if (name == "USER") return source::Participant::User;
    if (name == "MODEL") return source::Participant::Model;
    if (name == "ERROR") return source::Participant::Error;
    scenario_fail("unknown participant " + name);
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string stem = path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem + ": not an object");
    }
    const json::Value& root = *parsed.value;

    const std::string name = optional_text(root, "name").value_or(stem);
    const std::string op = required_text(root, "op");

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";

    if (op == "decode") {
        const std::optional<std::string> raw = optional_text(root, "raw");
        const std::optional<source::Source> decoded = source::decode(raw);
        trace += "args|" + opt(raw) + "\n";
        trace += "result|" + (decoded.has_value() ? esc(source::describe(decoded)) : "-") + "\n";
    } else if (op == "encode") {
        const std::optional<std::string> raw = optional_text(root, "raw");
        trace += "args|" + opt(raw) + "\n";
        trace += "result|" + opt(source::encode(source::decode(raw))) + "\n";
    } else if (op == "sanitize") {
        const std::optional<std::string> raw = optional_text(root, "raw");
        const std::string participant_name = required_text(root, "participant");
        const source::Participant participant = participant_from_name(participant_name);
        trace += "args|" + opt(raw) + "|" + esc(participant_name) + "\n";
        trace += "result|" + opt(source::sanitize_imported(raw, participant)) + "\n";
    } else if (op == "readable") {
        const json::Value* items_value = field(root, "items");
        std::vector<source::AskUserItem> items;
        if (items_value != nullptr && items_value->kind == json::Kind::Array) {
            for (const json::Value& element : items_value->elements) {
                if (element.kind != json::Kind::Object) scenario_fail(stem + ": item not object");
                source::AskUserItem item;
                item.question = required_text(element, "question");
                item.answer = optional_text(element, "answer");
                items.push_back(std::move(item));
            }
        }
        trace += "args|" + std::to_string(items.size());
        for (const source::AskUserItem& item : items) {
            trace += "|" + esc(item.question) + "|" +
                     (item.answer.has_value() ? esc(*item.answer) : "-");
        }
        trace += "\n";
        trace += "result|" + esc(source::ask_user_readable_text(items)) + "\n";
    } else if (op == "automation") {
        const std::optional<std::string> kind = optional_text(root, "requestKind");
        const std::optional<source::Source> resolved = source::for_automation_request_kind(kind);
        trace += "args|" + opt(kind) + "\n";
        trace += "result|" + (resolved.has_value() ? esc(source::describe(resolved)) : "-") + "\n";
    } else if (op == "constants") {
        source::Source task;
        task.kind = source::Kind::Task;
        source::Source loop;
        loop.kind = source::Kind::Loop;
        trace += "args|\n";
        trace += "result|" + esc(source::kNoAnswer) + "|" + esc(source::describe(task)) + "|" +
                 esc(source::describe(loop)) + "\n";
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

TEST(message_source_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MESSAGE_SOURCE_DIR;
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
