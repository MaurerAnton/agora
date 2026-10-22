// Golden parity test for the malformed tool call policy.
//
// The random UUID suffix is masked on both sides, so the stand-in request and the error text the
// model receives are compared exactly.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <regex>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/malformed_tool_call.hpp"
#include "check.hpp"

namespace {

namespace malformed = agora::core::malformed_tool;
namespace json = agora::core::json;

using json::Kind;
using json::Member;
using json::Value;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

[[noreturn]] void scenario_fail(const std::string& what) {
    throw agora::core::test::Failure(what);
}

const Value* field(const Value& object, std::string_view name) {
    if (object.kind != Kind::Object) return nullptr;
    const Value* found = nullptr;
    for (const Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

std::string required_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::String) {
        scenario_fail(std::string(name) + " must be a string");
    }
    return value->text;
}

std::optional<std::string> optional_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string");
    return value->text;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string mask_uuid(const std::string& text) {
    static const std::regex pattern("[0-9a-f]{32}");
    return std::regex_replace(text, pattern, "<uuid>");
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;

    // The port takes the entropy as a parameter; the stand masks it, so a fixed value is enough.
    const std::string suffix = "0123456789abcdef0123456789abcdef";

    std::string trace = "{\"requests\":[";
    if (const Value* requests = field(root, "requests");
        requests != nullptr && requests->kind == Kind::Array) {
        bool first = true;
        for (const Value& element : requests->elements) {
            const malformed::MalformedToolCallRequest request = malformed::malformed_tool_call_request(
                required_string(element, "cause"), optional_string(element, "originalName"),
                optional_string(element, "originalArguments"),
                optional_string(element, "streamKey"), optional_string(element, "signature"),
                suffix);
            if (!first) trace.push_back(',');
            first = false;
            trace += "{\"id\":";
            append_quoted(trace, mask_uuid(request.id));
            trace += ",\"name\":";
            append_quoted(trace, request.name);
            trace += ",\"arguments\":";
            append_quoted(trace, request.arguments);
            trace += ",\"signature\":";
            if (request.signature.has_value()) {
                append_quoted(trace, *request.signature);
            } else {
                trace += "null";
            }
            trace += ",\"streamKey\":";
            append_quoted(trace, mask_uuid(request.stream_key));
            trace.push_back('}');
        }
    }
    trace += "],\"results\":[";
    if (const Value* results = field(root, "results");
        results != nullptr && results->kind == Kind::Array) {
        bool first = true;
        for (const Value& element : results->elements) {
            if (!first) trace.push_back(',');
            first = false;
            append_quoted(trace, malformed::malformed_tool_call_result_text(element.text));
        }
    }
    trace += "]}";
    return trace;
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t at = 0;
    const std::size_t common = std::min(actual.size(), expected.size());
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 80 ? at - 80 : 0;
    const auto window = [&](const std::string& text) {
        return text.substr(from, std::min<std::size_t>(320, text.size() - from));
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << ", expected "
            << expected.size() << ")\n       actual:   " << window(actual)
            << "\n       expected: " << window(expected);
    return message.str();
}

}  // namespace

TEST(malformed_tool_call_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_MALFORMED_TOOL_CALL_DIR;
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
        std::string expected = read_file(golden_dir / (scenario.stem().string() + ".trace"));
        while (!expected.empty() && (expected.back() == '\n' || expected.back() == '\r')) {
            expected.pop_back();
        }
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
