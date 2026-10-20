// Golden parity test for the database-compatibility policy.
//
// Replays every scenario under core/tools/golden/database-compatibility/scenarios against the
// shipped port and requires the produced trace to equal the frozen Kotlin trace byte for byte.
// The trace format mirrors the reference driver
// (core/tools/golden/database-compatibility/DatabaseCompatibilityTraceDump.kt).
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

#include "agora/core/database_compatibility.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace db = agora::core::database_compatibility;
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

bool boolean(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value) || value->kind != json::Kind::Literal) return fallback;
    if (value->text == "true") return true;
    if (value->text == "false") return false;
    return fallback;
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

    if (op != "classify" && op != "can_open") {
        scenario_fail(stem + ": unknown op " + op);
    }

    const bool exists = boolean(root, "databaseExists", false);
    const int current = static_cast<int>(number(root, "currentVersion").value_or(0));
    const std::optional<std::int64_t> stored = number(root, "storedVersion");
    const std::optional<std::string> throw_class = optional_text(root, "throwClass");
    const std::optional<std::string> throw_message = optional_text(root, "throwMessage");

    std::optional<std::string> read_error;
    if (throw_class.has_value()) {
        std::string description = *throw_class;
        if (throw_message.has_value()) {
            description += ": ";
            description += *throw_message;
        }
        read_error = description;
    }

    std::optional<int> stored_version;
    if (stored.has_value()) stored_version = static_cast<int>(*stored);

    const db::Classification classification =
        db::classify(exists, current, stored_version, read_error);

    trace += "args|" + std::string(exists ? "true" : "false") + "|" + std::to_string(current) +
             "|" + (stored.has_value() ? std::to_string(*stored) : "-") + "|" +
             opt(throw_class) + "|" + opt(throw_message) + "\n";
    if (op == "classify") {
        trace += "result|" + esc(db::describe(classification)) + "\n";
    } else {
        trace += "result|" + std::string(db::can_open(classification) ? "true" : "false") + "\n";
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

TEST(database_compatibility_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_DATABASE_COMPATIBILITY_DIR;
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
