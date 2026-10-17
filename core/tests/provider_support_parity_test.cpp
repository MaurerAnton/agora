// Golden parity test for the C++ provider support logic (HTTP error mapping, base-URL
// versioning, provider defaults).
//
// Replays every scenario under core/tools/golden/provider-support/scenarios through the C++ port
// and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format and mode handling mirror the reference driver
// (core/tools/golden/provider-support/ProviderSupportTraceDump.kt) exactly.
#include <algorithm>
#include <charconv>
#include <filesystem>
#include <fstream>
#include <map>
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
namespace sn = agora::core::stream_normalization;

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

std::string or_null(const std::optional<std::string>& value) {
    return value ? esc(*value) : "-";
}

std::string flag(bool value) { return value ? "1" : "0"; }

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

std::int32_t int_or(const json::Value& object, std::string_view name, std::int32_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected an Int");
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value->text.data(),
                                        value->text.data() + value->text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
        scenario_fail(std::string(name), "expected an Int");
    }
    return parsed;
}

std::map<std::string, std::string> string_map(const json::Value* value) {
    std::map<std::string, std::string> out;
    if (value == nullptr || is_json_null(value)) return out;
    if (value->kind != json::Kind::Object) scenario_fail("map", "expected an object");
    for (const json::Member& entry : value->members) {
        if (entry.value.kind != json::Kind::String) scenario_fail("map", "expected string values");
        out[entry.key] = entry.value.text;
    }
    return out;
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
    const std::string mode = string_or(scenario, "mode", "httpError");

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

    if (mode == "httpError") {
        const std::int32_t status_code = int_or(scenario, "statusCode", 404);
        const std::optional<std::string> raw_body = optional_string(scenario, "rawBody");

        std::optional<support::ParsedProviderHttpErrorBody> parsed_body;
        if (raw_body) parsed_body = support::parse_provider_http_error_body(*raw_body);
        if (!parsed_body) {
            line({"parse", "none"});
        } else {
            line({"parse", "parsed",
                  "structured=" + flag(parsed_body->structured),
                  "code=" + or_null(parsed_body->code),
                  "type=" + or_null(parsed_body->type),
                  "message=" + esc(parsed_body->message)});
        }
        const sn::ErrorApi error = support::provider_http_error(status_code, raw_body);
        line({"http", "api", "code=" + or_null(error.code), "type=" + or_null(error.type),
              "message=" + esc(error.message)});
        std::optional<std::string> structured;
        if (raw_body) structured = support::extract_structured_provider_http_error_message(*raw_body);
        line({"structured", or_null(structured)});
    } else if (mode == "baseUrl") {
        const std::string url = string_or(scenario, "url", "");
        line({"hasVersion", flag(support::has_version_segment(url))});
        line({"withV1", esc(support::with_v1(url))});
        line({"withoutTrailing", or_null(support::without_trailing_version(url))});
    } else if (mode == "defaults") {
        line({"embedding", esc(support::embedding_base_url(
                               string_or(scenario, "embeddingProvider", "")))});
        line({"compatible",
              flag(support::is_openai_compatible_embedding(
                  string_or(scenario, "compatibleName", "")))});
        line({"openAiCompatible",
              esc(support::open_ai_compatible_base_url(
                  string_map(field(scenario, "openAiMap"))))});
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

TEST(provider_support_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_PROVIDER_SUPPORT_DIR;
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
