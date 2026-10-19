// Golden parity test for the C++ BoundedSemanticEmbeddingSelector port.
//
// Replays every scenario under core/tools/golden/embedding-selector/scenarios through
// agora::core::embedding::BoundedSemanticEmbeddingSelector and requires the produced trace to
// equal the frozen Kotlin trace byte for byte. The trace format mirrors the reference driver
// (core/tools/golden/embedding-selector/SelectorTraceDump.kt), including the loader contract
// checks, the invalid-row counters and the float spellings.
#include <algorithm>
#include <cstdlib>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/embedding_selector.hpp"
#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace embedding = agora::core::embedding;

using embedding::BoundedSemanticEmbeddingSelector;
using embedding::Candidate;
using embedding::SearchRow;
using embedding::Selection;

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

int int_or(const json::Value& object, std::string_view name, int fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected a number");
    return static_cast<int>(std::strtol(value->text.c_str(), nullptr, 10));
}

std::int64_t long_or(const json::Value& object, std::string_view name, std::int64_t fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind != json::Kind::Literal) scenario_fail(std::string(name), "expected a number");
    return std::strtoll(value->text.c_str(), nullptr, 10);
}

/** kotlinx JsonPrimitive.float: String.toFloat() on the raw text, non-finite values kept. */
float float_or(const json::Value& object, std::string_view name, float fallback) {
    const json::Value* value = field(object, name);
    if (is_null(value)) return fallback;
    if (value->kind == json::Kind::Literal || value->kind == json::Kind::String) {
        return std::strtof(value->text.c_str(), nullptr);
    }
    scenario_fail(std::string(name), "expected a number");
}

std::vector<float> float_list(const json::Value* value) {
    std::vector<float> out;
    if (is_null(value)) return out;
    if (value->kind != json::Kind::Array) return out;
    for (const json::Value& element : value->elements) {
        if (element.kind == json::Kind::Literal || element.kind == json::Kind::String) {
            out.push_back(std::strtof(element.text.c_str(), nullptr));
        }
    }
    return out;
}

std::string kotlin_float(float value) { return agora::core::openai::format_kotlin_float(value); }

std::string join_floats(const std::vector<float>& values) {
    std::string out;
    for (std::size_t i = 0; i < values.size(); ++i) {
        if (i != 0) out += ",";
        out += kotlin_float(values[i]);
    }
    return out;
}

/** The app stores vectors as big-endian IEEE-754, matching ByteBuffer.order(BIG_ENDIAN). */
std::vector<std::uint8_t> encode_floats(const std::vector<float>& values) {
    std::vector<std::uint8_t> bytes(values.size() * 4);
    for (std::size_t i = 0; i < values.size(); ++i) {
        std::uint32_t bits;
        std::memcpy(&bits, &values[i], sizeof(bits));
        bytes[i * 4 + 0] = static_cast<std::uint8_t>((bits >> 24) & 0xff);
        bytes[i * 4 + 1] = static_cast<std::uint8_t>((bits >> 16) & 0xff);
        bytes[i * 4 + 2] = static_cast<std::uint8_t>((bits >> 8) & 0xff);
        bytes[i * 4 + 3] = static_cast<std::uint8_t>(bits & 0xff);
    }
    return bytes;
}

struct ScenarioRow {
    std::int64_t id = 0;
    std::string message_id;
    int dimension = -1;
    std::vector<float> embedding;
};

ScenarioRow parse_row(const json::Value& value) {
    ScenarioRow row;
    row.id = long_or(value, "id", 0);
    row.message_id = string_or(value, "message_id", "");
    row.dimension = static_cast<int>(long_or(value, "dimension", -1));
    row.embedding = float_list(field(value, "embedding"));
    return row;
}

SearchRow to_row(const ScenarioRow& row, std::size_t query_size) {
    const int declared = row.dimension < 0 ? static_cast<int>(query_size) : row.dimension;
    return SearchRow{row.id, row.message_id, encode_floats(row.embedding), declared};
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
    const int page_size = int_or(scenario, "page_size", 256);
    const std::vector<float> query = float_list(field(scenario, "query"));
    const float threshold = float_or(scenario, "threshold", 0.0f);
    const int limit = int_or(scenario, "limit", 5);
    const std::string loader = string_or(scenario, "loader", "keyset");

    std::vector<ScenarioRow> rows;
    if (const json::Value* value = field(scenario, "rows")) {
        for (const json::Value& element : value->elements) rows.push_back(parse_row(element));
    }
    std::vector<std::vector<ScenarioRow>> raw_pages;
    if (const json::Value* value = field(scenario, "raw_pages")) {
        for (const json::Value& page : value->elements) {
            std::vector<ScenarioRow> parsed_page;
            for (const json::Value& element : page.elements) {
                parsed_page.push_back(parse_row(element));
            }
            raw_pages.push_back(parsed_page);
        }
    }

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
    line({"page_size", std::to_string(page_size)});
    line({"threshold", kotlin_float(threshold)});
    line({"limit", std::to_string(limit)});
    line({"query", join_floats(query)});

    try {
        const BoundedSemanticEmbeddingSelector selector(page_size);
        std::size_t call = 0;
        const Selection selection = selector.select(
            query, threshold, limit, [&](std::int64_t after_id, int page_limit) {
                std::vector<SearchRow> page;
                if (loader == "raw") {
                    if (call >= raw_pages.size()) return page;
                    for (const ScenarioRow& row : raw_pages[call++]) {
                        page.push_back(to_row(row, query.size()));
                    }
                    return page;
                }
                const int bound = loader == "oversized" ? page_limit + 1 : page_limit;
                for (const ScenarioRow& row : rows) {
                    if (row.id <= after_id) continue;
                    if (static_cast<int>(page.size()) >= bound) break;
                    page.push_back(to_row(row, query.size()));
                }
                return page;
            });

        line({"candidates", std::to_string(selection.candidates.size())});
        for (const Candidate& candidate : selection.candidates) {
            line({"candidate", std::to_string(candidate.row_id), esc(candidate.message_id),
                  kotlin_float(candidate.score)});
        }
        line({"scanned", std::to_string(selection.scanned_rows)});
        line({"skipped", std::to_string(selection.skipped_invalid_rows)});
        line({"best_score", kotlin_float(selection.best_score)});
        line({"max_retained", std::to_string(selection.max_retained_candidates)});
    } catch (const std::exception& error) {
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

TEST(embedding_selector_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_EMBEDDING_SELECTOR_DIR;
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
