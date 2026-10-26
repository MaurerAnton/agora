// Golden parity test for the pure file helpers in util/ShellClient.kt.
//
// Replays every scenario under core/tools/golden/shell-file-edit/scenarios through the C++ port
// and requires the produced trace to equal the frozen Kotlin trace byte for byte. The Kotlin
// stand extracts the declarations verbatim from ShellClient.kt; this side drives the same policy
// without Conch, SSH, sockets or Android IO.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <limits>
#include <optional>
#include <sstream>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/shell_file_edit.hpp"
#include "agora/core/shell_tools.hpp"
#include "check.hpp"
#include "utf16.hpp"

namespace {

namespace shell_file = agora::core::shell_file;
namespace shell_tools = agora::core::shell_tools;
namespace json = agora::core::json;
namespace utf16 = agora::core::utf16;

constexpr std::int64_t kDefaultShellOutputMaxBytes = 1 << 20;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

// ---- JSON value construction -------------------------------------------------------------------

json::Value string_value(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value literal_value(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value number_value(std::int64_t number) { return literal_value(std::to_string(number)); }

json::Value bool_value(bool value) { return literal_value(value ? "true" : "false"); }

json::Value null_value() { return literal_value("null"); }

json::Value array_value(std::vector<json::Value> elements) {
    json::Value value;
    value.kind = json::Kind::Array;
    value.elements = std::move(elements);
    return value;
}

json::Value object_value(std::vector<std::pair<std::string, json::Value>> members) {
    json::Value value;
    value.kind = json::Kind::Object;
    for (auto& member : members) {
        value.members.push_back(json::Member{std::move(member.first), std::move(member.second)});
    }
    return value;
}

json::Value error_value(const std::string& type, const std::string& message) {
    return object_value({
        {"error", object_value({
                      {"type", string_value(type)},
                      {"message", string_value(message)},
                  })},
    });
}

// ---- scenario parsing --------------------------------------------------------------------------

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) return &entry.value;
    }
    return nullptr;
}

std::string required_string(const json::Value& object, std::string_view name,
                            const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::String) {
        scenario_fail(scenario, "missing string " + std::string(name));
    }
    return value->text;
}

bool required_bool(const json::Value& object, std::string_view name,
                   const std::string& scenario) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal ||
        (value->text != "true" && value->text != "false")) {
        scenario_fail(scenario, "missing boolean " + std::string(name));
    }
    return value->text == "true";
}

std::optional<std::int64_t> optional_long(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || value->kind != json::Kind::Literal) return std::nullopt;
    try {
        return std::stoll(value->text);
    } catch (const std::exception&) {
        return std::nullopt;
    }
}

std::int64_t required_long(const json::Value& object, std::string_view name,
                           const std::string& scenario) {
    const std::optional<std::int64_t> value = optional_long(object, name);
    if (!value) scenario_fail(scenario, "missing number " + std::string(name));
    return *value;
}

std::vector<std::int64_t> number_list(const json::Value& object, std::string_view name,
                                      const std::string& scenario) {
    std::vector<std::int64_t> out;
    const json::Value* value = field(object, name);
    if (value == nullptr) return out;
    if (value->kind != json::Kind::Array) {
        scenario_fail(scenario, std::string(name) + " is not an array");
    }
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::Literal) {
            scenario_fail(scenario, std::string(name) + " element is not a number");
        }
        try {
            out.push_back(std::stoll(element.text));
        } catch (const std::exception&) {
            scenario_fail(scenario, std::string(name) + " element is not a number");
        }
    }
    return out;
}

struct RangeSpec {
    std::int64_t first = 0;
    std::int64_t last = 0;
};

std::vector<RangeSpec> range_list(const json::Value& object, std::string_view name,
                                  const std::string& scenario) {
    std::vector<RangeSpec> out;
    const json::Value* value = field(object, name);
    if (value == nullptr) return out;
    if (value->kind != json::Kind::Array) {
        scenario_fail(scenario, std::string(name) + " is not an array");
    }
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::Array || element.elements.size() != 2) {
            scenario_fail(scenario, std::string(name) + " element is not a pair");
        }
        try {
            out.push_back({std::stoll(element.elements[0].text),
                           std::stoll(element.elements[1].text)});
        } catch (const std::exception&) {
            scenario_fail(scenario, std::string(name) + " element is not a numeric pair");
        }
    }
    return out;
}

int hex_digit(char c) {
    if (c >= '0' && c <= '9') return c - '0';
    if (c >= 'a' && c <= 'f') return c - 'a' + 10;
    if (c >= 'A' && c <= 'F') return c - 'A' + 10;
    return -1;
}

std::string from_hex(std::string_view hex, const std::string& scenario) {
    if (hex.size() % 2 != 0) scenario_fail(scenario, "odd hex length");
    std::string out;
    out.reserve(hex.size() / 2);
    for (std::size_t i = 0; i < hex.size(); i += 2) {
        const int high = hex_digit(hex[i]);
        const int low = hex_digit(hex[i + 1]);
        if (high < 0 || low < 0) scenario_fail(scenario, "invalid hex digit");
        out.push_back(static_cast<char>((high << 4) | low));
    }
    return out;
}

// ---- trace fragments ---------------------------------------------------------------------------

json::Value numbers_value(const std::vector<std::int64_t>& values) {
    std::vector<json::Value> elements;
    elements.reserve(values.size());
    for (const std::int64_t value : values) elements.push_back(number_value(value));
    return array_value(std::move(elements));
}

json::Value ranges_value(const std::vector<RangeSpec>& ranges) {
    std::vector<json::Value> elements;
    elements.reserve(ranges.size());
    for (const RangeSpec& range : ranges) {
        elements.push_back(array_value({number_value(range.first), number_value(range.last)}));
    }
    return array_value(std::move(elements));
}

std::vector<shell_file::Utf16Range> to_ranges(const std::vector<RangeSpec>& specs,
                                              const std::string& scenario) {
    std::vector<shell_file::Utf16Range> out;
    out.reserve(specs.size());
    for (const RangeSpec& spec : specs) {
        if (spec.first < 0 || spec.last < 0) {
            scenario_fail(scenario, "negative match range");
        }
        out.push_back({static_cast<std::size_t>(spec.first),
                       static_cast<std::size_t>(spec.last)});
    }
    return out;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 48 ? at - 48 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(160, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

// ---- replay ------------------------------------------------------------------------------------

std::string run_scenario(const json::Value& scenario, const std::string& op,
                         const std::string& stem) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("op", string_value(op));
    json::Value outcome;

    if (op == "lineCount") {
        const std::string content = required_string(scenario, "content", stem);
        members.emplace_back("content", string_value(content));
        outcome = object_value({
            {"count", number_value(shell_tools::shell_file_line_count(content))},
        });
    } else if (op == "sha256") {
        const std::string bytes_hex = required_string(scenario, "bytesHex", stem);
        members.emplace_back("bytesHex", string_value(bytes_hex));
        outcome = object_value({
            {"sha256", string_value(shell_file::shell_file_sha256(from_hex(bytes_hex, stem)))},
        });
    } else if (op == "completeUtf8Prefix") {
        const std::string bytes_hex = required_string(scenario, "bytesHex", stem);
        members.emplace_back("bytesHex", string_value(bytes_hex));
        outcome = object_value({
            {"length",
             number_value(static_cast<std::int64_t>(
                 shell_file::complete_utf8_prefix_length(from_hex(bytes_hex, stem))))},
        });
    } else if (op == "readBoundedShellOutput") {
        const std::string bytes_hex = required_string(scenario, "bytesHex", stem);
        const std::optional<std::int64_t> explicit_max = optional_long(scenario, "maxBytes");
        const std::int64_t max_bytes = explicit_max.value_or(kDefaultShellOutputMaxBytes);
        if (max_bytes < std::numeric_limits<int>::min() ||
            max_bytes > std::numeric_limits<int>::max()) {
            scenario_fail(stem, "maxBytes is outside Int range");
        }
        const std::vector<std::int64_t> chunks = number_list(scenario, "chunks", stem);
        members.emplace_back("bytesHex", string_value(bytes_hex));
        members.emplace_back("maxBytes", number_value(max_bytes));
        members.emplace_back("chunks", numbers_value(chunks));
        try {
            auto [text, truncated] = shell_file::read_bounded_shell_output(
                from_hex(bytes_hex, stem), static_cast<int>(max_bytes));
            outcome = object_value({
                {"content", string_value(utf16::to_utf8(text))},
                {"truncated", bool_value(truncated)},
            });
        } catch (const std::invalid_argument& error) {
            outcome = error_value("IllegalArgumentException", error.what());
        }
    } else if (op == "matchRanges") {
        const std::string content = required_string(scenario, "content", stem);
        const std::string old_string = required_string(scenario, "oldString", stem);
        members.emplace_back("content", string_value(content));
        members.emplace_back("oldString", string_value(old_string));
        try {
            const std::vector<shell_file::Utf16Range> ranges =
                shell_file::file_edit_match_ranges(utf16::from_utf8(content),
                                                   utf16::from_utf8(old_string));
            std::vector<RangeSpec> specs;
            specs.reserve(ranges.size());
            for (const shell_file::Utf16Range& range : ranges) {
                specs.push_back({static_cast<std::int64_t>(range.first),
                                 static_cast<std::int64_t>(range.last)});
            }
            outcome = object_value({{"ranges", ranges_value(specs)}});
        } catch (const std::invalid_argument& error) {
            outcome = error_value("IllegalArgumentException", error.what());
        }
    } else if (op == "replaceMatches") {
        const std::string content = required_string(scenario, "content", stem);
        const std::string new_string = required_string(scenario, "newString", stem);
        const std::vector<RangeSpec> specs = range_list(scenario, "matches", stem);
        members.emplace_back("content", string_value(content));
        members.emplace_back("matches", ranges_value(specs));
        members.emplace_back("newString", string_value(new_string));
        try {
            const std::u16string replaced = shell_file::replace_file_edit_matches(
                utf16::from_utf8(content), to_ranges(specs, stem), utf16::from_utf8(new_string));
            outcome = object_value({{"content", string_value(utf16::to_utf8(replaced))}});
        } catch (const std::invalid_argument& error) {
            outcome = error_value("IllegalArgumentException", error.what());
        }
    } else if (op == "edit") {
        const std::string content = required_string(scenario, "content", stem);
        const std::string old_string = required_string(scenario, "oldString", stem);
        const std::string new_string = required_string(scenario, "newString", stem);
        const bool replace_all = required_bool(scenario, "replaceAll", stem);
        members.emplace_back("content", string_value(content));
        members.emplace_back("oldString", string_value(old_string));
        members.emplace_back("newString", string_value(new_string));
        members.emplace_back("replaceAll", bool_value(replace_all));
        try {
            auto [edited, result] = shell_file::edit_shell_file_content(
                utf16::from_utf8(content), utf16::from_utf8(old_string),
                utf16::from_utf8(new_string), replace_all);
            outcome = object_value({
                {"content", edited ? string_value(utf16::to_utf8(*edited)) : null_value()},
                {"result", object_value({
                               {"replacements", number_value(result.replacements)},
                               {"sha256", string_value(result.sha256)},
                               {"error", result.error ? string_value(*result.error) : null_value()},
                           })},
            });
        } catch (const std::invalid_argument& error) {
            outcome = error_value("IllegalArgumentException", error.what());
        }
    } else if (op == "preferredLineEnding") {
        const std::string text = required_string(scenario, "text", stem);
        members.emplace_back("text", string_value(text));
        const std::optional<std::u16string> line_ending =
            shell_file::preferred_file_edit_line_ending(utf16::from_utf8(text));
        outcome = object_value({
            {"lineEnding",
             line_ending ? string_value(utf16::to_utf8(*line_ending)) : null_value()},
        });
    } else if (op == "withLineEnding") {
        const std::string text = required_string(scenario, "text", stem);
        const std::string line_ending = required_string(scenario, "lineEnding", stem);
        members.emplace_back("text", string_value(text));
        members.emplace_back("lineEnding", string_value(line_ending));
        const std::u16string result = shell_file::with_file_edit_line_ending(
            utf16::from_utf8(text), utf16::from_utf8(line_ending));
        outcome = object_value({{"content", string_value(utf16::to_utf8(result))}});
    } else {
        scenario_fail(stem, "unknown op: " + op);
    }

    members.emplace_back("outcome", std::move(outcome));
    return json::to_kotlin_string(object_value(std::move(members))) + "\n";
}

}  // namespace

TEST(shell_file_edit_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SHELL_FILE_EDIT_DIR;
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
    for (const std::filesystem::path& scenario_path : scenarios) {
        const std::string stem = scenario_path.stem().string();
        const json::ParseResult parsed = json::parse(read_file(scenario_path));
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            scenario_fail(stem, "scenario is not a JSON object");
        }
        const std::string op = required_string(*parsed.value, "op", stem);
        const std::string actual = run_scenario(*parsed.value, op, stem);
        const std::string expected = read_file(golden_dir / (stem + ".trace"));
        CHECK(!expected.empty());
        if (actual != expected) {
            throw agora::core::test::Failure(stem + ": " + describe_mismatch(actual, expected));
        }
        ++checked;
    }
    CHECK(checked == static_cast<int>(scenarios.size()));
}
