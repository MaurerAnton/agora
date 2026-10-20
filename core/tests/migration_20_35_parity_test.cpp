// Golden parity test for the SQL-only v20..v35 schema migrations.
//
// Each scenario names one source version. The C++ Database shim records every execSQL call and
// the trace (calls JSON + exception) must equal the frozen Kotlin trace byte for byte.
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
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/migration_20_35.hpp"
#include "check.hpp"

namespace {

namespace migrations = agora::core::migration_20_35;
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

std::optional<std::string> optional_text(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || (value->kind == json::Kind::Literal && value->text == "null")) {
        return std::nullopt;
    }
    if (value->kind != json::Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

std::string required_text(const json::Value& object, std::string_view name) {
    const std::optional<std::string> value = optional_text(object, name);
    if (!value.has_value()) scenario_fail(std::string(name) + " is required");
    return *value;
}

std::int64_t required_number(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) {
        scenario_fail(std::string(name) + " is required");
    }
    try {
        std::size_t consumed = 0;
        const std::int64_t parsed = std::stoll(value->text, &consumed);
        if (consumed != value->text.size()) scenario_fail(std::string(name) + " is not an integer");
        return parsed;
    } catch (const std::exception&) {
        scenario_fail(std::string(name) + " is not an integer");
    }
}

std::string render_arg(const migrations::SqlValue& value) {
    switch (value.kind) {
        case migrations::SqlValue::Kind::Null: return "null";
        case migrations::SqlValue::Kind::Text: return "s:" + value.text;
        case migrations::SqlValue::Kind::Long: return "l:" + std::to_string(value.number);
        case migrations::SqlValue::Kind::Int: return "i:" + std::to_string(value.number);
    }
    return "null";
}

class RecordingDatabase : public migrations::Database {
public:
    std::vector<std::vector<std::string>> calls;

    void exec_sql(std::string_view sql, const std::vector<migrations::SqlValue>& args) override {
        std::vector<std::string> call;
        call.emplace_back("execSQL");
        call.emplace_back(sql);
        for (const migrations::SqlValue& arg : args) call.push_back(render_arg(arg));
        calls.push_back(std::move(call));
    }

    migrations::QueryResult query(std::string_view sql,
                                  const std::vector<migrations::SqlValue>&) override {
        throw std::runtime_error("unexpected query: " + std::string(sql));
    }
};

std::string render_calls(const std::vector<std::vector<std::string>>& calls) {
    json::Value array;
    array.kind = json::Kind::Array;
    for (const std::vector<std::string>& call : calls) {
        json::Value call_value;
        call_value.kind = json::Kind::Array;
        for (const std::string& part : call) {
            json::Value text;
            text.kind = json::Kind::String;
            text.text = part;
            call_value.elements.push_back(std::move(text));
        }
        array.elements.push_back(std::move(call_value));
    }
    return json::to_kotlin_string(array);
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
    if (op != "migrate") scenario_fail(stem + ": unknown op " + op);
    const int version = static_cast<int>(required_number(root, "version"));

    std::string trace = "name|" + esc(name) + "\n";
    trace += "op|" + esc(op) + "\n";
    trace += "version|" + std::to_string(version) + "\n";

    RecordingDatabase database;
    std::string error = "-";
    try {
        if (!migrations::migrate(version, database)) {
            scenario_fail(stem + ": unknown source version " + std::to_string(version));
        }
    } catch (const migrations::MigrationException& exception) {
        error = exception.class_name + ": " + exception.message;
    }
    trace += "result|" + esc(render_calls(database.calls)) + "|" + esc(error) + "\n";
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

TEST(migration_20_35_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MIGRATION_20_35_DIR;
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
