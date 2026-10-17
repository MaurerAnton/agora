// Golden parity test for the C++ DuckDuckGo scraper port.
//
// Replays every scenario under core/tools/golden/duckduckgo/scenarios through the C++ port
// (parse_results / strip_html / extract_url and the search loop over an injected page source)
// and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format, escaping and mode handling mirror the reference driver
// (core/tools/golden/duckduckgo/DuckDuckGoTraceDump.kt) exactly.
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

#include "agora/core/duckduckgo.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace ddg = agora::core::duckduckgo;

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

std::size_t size_or(const json::Value& object, std::string_view name, std::size_t fallback) {
    const json::Value* value = field(object, name);
    if (value == nullptr || is_json_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected an Int");
    std::int32_t parsed = 0;
    const auto result = std::from_chars(value->text.data(),
                                        value->text.data() + value->text.size(), parsed);
    if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size() ||
        parsed < 0) {
        scenario_fail(std::string(name), "expected a non-negative Int");
    }
    return static_cast<std::size_t>(parsed);
}

// -- scenario runner ---------------------------------------------------------------------------------

std::string result_line(const ddg::WebResult& result) {
    return "result|" + esc(result.title) + "|" + esc(result.url) + "|" + esc(result.snippet);
}

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
    const std::string mode = string_or(scenario, "mode", "search");
    const std::size_t max_results = size_or(scenario, "maxResults", 5);

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

    if (mode == "parse") {
        const std::string html = string_or(scenario, "html", "");
        for (const ddg::WebResult& result : ddg::parse_results(html, max_results)) {
            out += result_line(result);
            out += '\n';
        }
        return out;
    }
    if (mode != "search") scenario_fail(stem, "unknown mode: " + mode);

    const json::Value* pages_value = field(scenario, "pages");
    if (pages_value == nullptr || pages_value->kind != json::Kind::Array) {
        scenario_fail(stem, "pages must be an array");
    }
    const std::vector<json::Value> pages = pages_value->elements;

    // The scenario page list is the transport seam (the real fetchPage feeds OkHttp); running
    // out of pages behaves like the fetch returning a null body, exactly like the driver.
    std::size_t fetch_index = 0;
    const ddg::PageSource page_source = [&](std::string_view, int, const std::string&) {
        ddg::PageResult result;
        if (fetch_index >= pages.size()) {
            result.failed = true;
            result.message = "Empty response body from DuckDuckGo.";
        } else {
            const json::Value& page = pages[fetch_index];
            ++fetch_index;
            if (page.kind != json::Kind::Object) scenario_fail(stem, "pages must be objects");
            if (const auto error = optional_string(page, "error")) {
                result.failed = true;
                result.message = *error;
            } else if (const auto html = optional_string(page, "html")) {
                result.html = *html;
            } else {
                result.failed = true;
                result.message = "Empty response body from DuckDuckGo.";
            }
        }
        return result;
    };

    const ddg::SearchOutcome outcome = ddg::search("", max_results, page_source);
    if (outcome.success) {
        for (const ddg::WebResult& result : outcome.results) {
            out += result_line(result);
            out += '\n';
        }
        line({"response", "success"});
    } else {
        line({"response", "error", ddg::search_error_type_name(outcome.error_type),
              esc(outcome.error_message)});
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

TEST(duckduckgo_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_DUCKDUCKGO_DIR;
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
