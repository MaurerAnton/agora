// Golden parity test for the C++ Conch SSE parser.
//
// Replays every scenario under core/tools/golden/conch-sse/scenarios through
// agora::core::conch::parse_conch_sse_lines and requires the produced trace to equal the frozen
// Kotlin trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/conch-sse/ConchSseTraceDump.kt): the deltas emitted while streaming come
// before the terminal result or the failing message, because the parser is allowed to flush
// output before the frame that fails closed.
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conch_sse.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace conch = agora::core::conch;

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

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) return &entry.value;
    }
    return nullptr;
}

bool is_null(const json::Value* value) {
    return value == nullptr || (value->kind == json::Kind::Literal && value->text == "null");
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a string");
    return value->text;
}

bool bool_or(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::Literal || (value->text != "true" && value->text != "false")) {
        scenario_fail(std::string(name), "expected a boolean");
    }
    return value->text == "true";
}

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const json::ParseResult parsed = json::parse(read_file(scenario_path));
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string name = string_or(scenario, "name", stem);
    if (name != stem) {
        scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    }
    const bool encrypted = bool_or(scenario, "encrypted", false);
    const std::optional<std::string> decrypt_error =
        is_null(field(scenario, "decrypt_error"))
            ? std::nullopt
            : std::optional<std::string>(string_or(scenario, "decrypt_error", ""));

    std::vector<std::string> lines;
    if (const json::Value* value = field(scenario, "lines")) {
        for (const json::Value& element : value->elements) {
            if (element.kind != json::Kind::String) {
                scenario_fail(stem, "every line must be a JSON string");
            }
            lines.push_back(element.text);
        }
    }

    std::vector<std::string> emitted;
    std::size_t cursor = 0;

    std::ostringstream trace;
    const auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) trace << '|';
            first = false;
            trace << value;
        }
        trace << '\n';
    };

    line({"name", esc(name)});
    line({"encrypted", encrypted ? "1" : "0"});

    try {
        const conch::SseResult result = conch::parse_conch_sse_lines(
            encrypted,
            [&]() -> std::optional<std::string> {
                if (cursor >= lines.size()) return std::nullopt;
                return lines[cursor++];
            },
            [&](const std::string& text) -> std::string {
                if (decrypt_error) throw std::runtime_error(*decrypt_error);
                return text;
            },
            [&](const std::string& delta) { emitted.push_back(delta); });

        for (const std::string& delta : emitted) line({"emit", esc(delta)});
        line({"output", esc(result.output)});
        line({"exit_code", result.exit_code ? std::to_string(*result.exit_code) : std::string("-")});
        line({"error_message",
              result.error_message ? esc(*result.error_message) : std::string("-")});
        line({"timed_out", result.timed_out ? "1" : "0"});
        line({"warning", result.warning_message ? esc(*result.warning_message) : std::string("-")});
    } catch (const std::exception& error) {
        for (const std::string& delta : emitted) line({"emit", esc(delta)});
        line({"error", esc(error.what())});
    }
    return trace.str();
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text, std::size_t begin) {
        return text.substr(begin, std::min<std::size_t>(160, text.size() - begin));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual, from)
            << "\n       expected: " << window(expected, from);
    return message.str();
}

}  // namespace

TEST(conch_sse_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CONCH_SSE_DIR;
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
