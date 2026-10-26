// Golden parity test for the pure sandbox virtual-path helpers.
//
// SandboxVirtualPaths.kt and PortableGlobMatcher.kt compile verbatim from the app tree; this
// side replays every frozen scenario through the C++ port and compares the canonical trace byte
// for byte. java.io canonicalization and the mount traversal check remain platform concerns.
#include <algorithm>
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
#include "agora/core/sandbox_virtual_paths.hpp"
#include "check.hpp"

namespace {

namespace sandbox = agora::core::sandbox;
namespace json = agora::core::json;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

json::Value string_value(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

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

std::vector<std::string> string_list(const json::Value& object, std::string_view name,
                                     const std::string& scenario) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr) return out;
    if (value->kind != json::Kind::Array) {
        scenario_fail(scenario, std::string(name) + " is not an array");
    }
    for (const json::Value& element : value->elements) {
        if (element.kind != json::Kind::String) {
            scenario_fail(scenario, std::string(name) + " element is not a string");
        }
        out.push_back(element.text);
    }
    return out;
}

json::Value strings_value(const std::vector<std::string>& values) {
    std::vector<json::Value> elements;
    elements.reserve(values.size());
    for (const std::string& value : values) elements.push_back(string_value(value));
    return array_value(std::move(elements));
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

std::string run_scenario(const json::Value& scenario, const std::string& op,
                         const std::string& stem) {
    std::vector<std::pair<std::string, json::Value>> members;
    members.emplace_back("op", string_value(op));
    json::Value outcome;

    if (op == "normalize") {
        const std::string path = required_string(scenario, "path", stem);
        members.emplace_back("path", string_value(path));
        outcome = object_value({
            {"normalized", string_value(sandbox::normalize_virtual_path(path))},
        });
    } else if (op == "glob") {
        const std::vector<std::string> files = string_list(scenario, "files", stem);
        const std::string pattern = required_string(scenario, "pattern", stem);
        members.emplace_back("files", strings_value(files));
        members.emplace_back("pattern", string_value(pattern));
        outcome = object_value({
            {"files", strings_value(sandbox::glob_match(files, pattern))},
        });
    } else {
        scenario_fail(stem, "unknown op: " + op);
    }

    members.emplace_back("outcome", std::move(outcome));
    return json::to_kotlin_string(object_value(std::move(members))) + "\n";
}

}  // namespace

TEST(sandbox_virtual_paths_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_SANDBOX_VIRTUAL_PATHS_DIR;
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
