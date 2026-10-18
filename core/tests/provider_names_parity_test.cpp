// Golden parity test for the C++ custom provider name policy.
//
// Replays every scenario under core/tools/golden/provider-names/scenarios through
// agora::core::provider_support and requires the produced trace to equal the frozen Kotlin trace
// byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/provider-names/ProviderNamesTraceDump.kt).
#include <algorithm>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/provider_support.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace support = agora::core::provider_support;

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

bool is_json_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

std::optional<std::string> optional_string(const json::Value& object, std::string_view name) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return std::nullopt;
    if (value->kind != json::Kind::String) scenario_fail(std::string(name), "expected a string");
    return value->text;
}

std::string string_or(const json::Value& object, std::string_view name,
                      const std::string& fallback) {
    const auto value = optional_string(object, name);
    return value ? *value : fallback;
}

std::vector<std::string> string_list_or(const json::Value& object, std::string_view name) {
    std::vector<std::string> out;
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return out;
    if (value->kind != json::Kind::Array) scenario_fail(std::string(name), "expected an array");
    for (const json::Value& entry : value->elements) {
        if (entry.kind != json::Kind::String) scenario_fail(std::string(name), "expected strings");
        out.push_back(entry.text);
    }
    return out;
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
    const std::string mode = string_or(scenario, "mode", "allowed");
    const std::string provider_name = string_or(scenario, "providerName", "");

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

    if (mode == "allowed") {
        line({"allowed", support::is_allowed_custom_provider_name(provider_name) ? "1" : "0"});
    } else if (mode == "conflict") {
        line({"conflict", support::has_custom_provider_name_conflict(
                                   provider_name, string_list_or(scenario, "existingNames"),
                                   optional_string(scenario, "currentName"))
                                   ? "1"
                                   : "0"});
    } else if (mode == "sanitize") {
        const support::CustomProviderSanitizationResult result =
            support::sanitize_custom_provider_names(string_list_or(scenario, "configs"));
        line({"accepted", std::to_string(result.accepted.size())});
        for (std::size_t i = 0; i < result.accepted.size(); ++i) {
            line({"accept", std::to_string(i), esc(result.accepted[i])});
        }
        line({"rejected", std::to_string(result.rejected.size())});
        for (std::size_t i = 0; i < result.rejected.size(); ++i) {
            line({"reject", std::to_string(i), esc(result.rejected[i])});
        }
    } else {
        scenario_fail(stem, "unknown mode: " + mode);
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

TEST(provider_names_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PROVIDER_NAMES_DIR;
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