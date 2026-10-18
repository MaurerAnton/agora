// Golden parity test for the C++ shell-tools port.
//
// Replays every scenario under core/tools/golden/shell-tools/scenarios through the C++ port
// (parse_tool_args / arg / bool_arg / json_error / bounded_file_read_json) and requires the
// produced trace to equal the frozen Kotlin trace byte for byte. The trace format and mode
// handling mirror the reference driver (core/tools/golden/shell-tools/ShellToolsTraceDump.kt).
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_tools.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace shell = agora::core::shell_tools;

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

// -- scenario decoding ------------------------------------------------------------------------------

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

bool is_json_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) {
        scenario_fail(std::string(name), "expected a JSON string");
    }
    return value->text;
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const auto value = optional_string(object, name);
    return value ? *value : fallback;
}

std::int64_t int64_or(const json::Value& object, std::string_view name, std::int64_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected an Int");
    std::int64_t parsed = 0;
    const auto result = std::from_chars(value->text.data(),
                                        value->text.data() + value->text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected an Int");
    }
    return parsed;
}

std::optional<std::int32_t> optional_int(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    return static_cast<std::int32_t>(int64_or(object, name, 0));
}

bool bool_or(const json::Value& object, std::string_view name, bool fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal || (value->text != "true" && value->text != "false")) {
        scenario_fail(std::string(name), "expected a JSON boolean");
    }
    return value->text == "true";
}

// -- scenario runner ---------------------------------------------------------------------------------

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const std::string source = read_file(scenario_path);
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const std::string name = string_or(scenario, "name", stem);
    if (name != stem) {
        scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    }
    const std::string mode = string_or(scenario, "mode", "toolArgs");

    std::string out;
    const auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) out += '|';
            first = false;
            out += value;
        }
        out += '\n';
    };
    line({"name", esc(name)});
    line({"mode", esc(mode)});

    try {
        if (mode == "toolArgs") {
            const std::vector<std::pair<std::string, json::Value>> args =
                shell::parse_tool_args(string_or(scenario, "arguments", ""));
            for (const auto& [key, value] : args) {
                line({"arg", esc(key), esc(json::to_kotlin_string(value))});
            }
            const std::string probe_key = string_or(scenario, "probeKey", "");
            line({"probe", esc(shell::arg(args, probe_key))});
            line({"bool", shell::bool_arg(args, probe_key) ? "1" : "0"});
        } else if (mode == "jsonError") {
            line({"json", esc(shell::json_error(string_or(scenario, "type", ""),
                                                string_or(scenario, "message", ""),
                                                optional_string(scenario, "server"),
                                                optional_string(scenario, "command")))});
        } else if (mode == "fileRead") {
            shell::ShellFileReadResult result;
            result.content = string_or(scenario, "fileContent", "");
            result.lines = static_cast<std::int32_t>(int64_or(scenario, "fileLines", 0));
            result.total_lines = static_cast<std::int32_t>(int64_or(scenario, "totalLines", 0));
            result.total_bytes = int64_or(scenario, "totalBytes", 0);
            result.returned_bytes = 0;
            result.offset = int64_or(scenario, "offset", 0);
            result.limit = int64_or(scenario, "limit", 0);
            result.truncated = bool_or(scenario, "fileTruncated", false);
            const std::string server = string_or(scenario, "fileServer", "");
            const std::string path = string_or(scenario, "filePath", "");
            const std::optional<std::int32_t> max_length = optional_int(scenario, "maxLength");
            const std::string rendered =
                max_length ? shell::bounded_file_read_json(server, path, result, *max_length)
                           : shell::bounded_file_read_json(server, path, result);
            line({"json", esc(rendered)});
        } else {
            scenario_fail(stem, "unknown mode: " + mode);
        }
    } catch (const agora::core::openai::ProviderRequestError& error) {
        line({"error", error.class_name});
    } catch (const agora::core::test::Failure&) {
        throw;
    } catch (const std::exception& error) {
        out += "error|";
        out += error.what();
        out += "|\n";
    }
    return out;
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

TEST(shell_tools_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_TOOLS_DIR;
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
        checked++;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
