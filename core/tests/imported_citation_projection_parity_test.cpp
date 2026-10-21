// Golden parity test for ImportedCitationProjection (ChatGPT/Claude citation import).
//
// Every scenario replays the real Kotlin trace: the marker scan over UTF-16 indexes, the
// reference-walking order, the three anchor-resolution branches, CitationPolicy dedup/create and
// the bounded encode. CitationRecord is compared field by field, nulls included.
#include <algorithm>
#include <cctype>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/imported_citation_projection.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace projection = agora::core::imported_citation;
namespace json = agora::core::json;
namespace citations = agora::core::citations;

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
        if (entry.key == name) found = &entry.value;  // last duplicate wins
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

int required_int(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr || value->kind != Kind::Literal) {
        scenario_fail(std::string(name) + " must be an int");
    }
    const std::string& text = value->text;
    std::size_t i = 0;
    bool negative = false;
    if (i < text.size() && (text[i] == '-' || text[i] == '+')) {
        negative = text[i] == '-';
        ++i;
    }
    long long parsed = 0;
    for (; i < text.size(); ++i) {
        if (text[i] < '0' || text[i] > '9') scenario_fail(std::string(name) + " is not an int");
        parsed = parsed * 10 + (text[i] - '0');
        if (parsed > 2147483648LL) scenario_fail(std::string(name) + " overflows Int");
    }
    if (negative) parsed = -parsed;
    if (parsed < -2147483648LL || parsed > 2147483647LL) {
        scenario_fail(std::string(name) + " overflows Int");
    }
    return static_cast<int>(parsed);
}

std::optional<std::string> nullable_string(const Value& object, std::string_view name) {
    const Value* value = field(object, name);
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::Literal && value->text == "null") return std::nullopt;
    if (value->kind != Kind::String) scenario_fail(std::string(name) + " must be a string?");
    return value->text;
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

std::string quoted(std::string_view text) {
    std::string out;
    append_quoted(out, text);
    return out;
}

// ------------------------------------------------------------------------- scenario decoding

/** Driver-side decode of the injected CitationRecord list (the real serializer applies the
 *  declared defaults: version = 1, nullable fields = null, anchors = []). */
citations::CitationRecord decode_record(const Value& value) {
    if (value.kind != Kind::Object) scenario_fail("citation records must be objects");
    citations::CitationRecord record;
    if (const Value* version = field(value, "version")) {
        if (version->kind != Kind::Literal) scenario_fail("version must be an int");
        const std::string text = version->text;
        if (text != "1") scenario_fail("only version 1 exists in the corpus");
        record.version = 1;
    }
    record.source_id = required_string(value, "sourceId");
    record.provider = required_string(value, "provider");
    record.kind = required_string(value, "kind");
    record.title = required_string(value, "title");
    record.url = nullable_string(value, "url");
    record.file_name = nullable_string(value, "fileName");
    record.location = nullable_string(value, "location");
    record.provider_source_id = nullable_string(value, "providerSourceId");
    record.excerpt = nullable_string(value, "excerpt");
    if (const Value* anchors = field(value, "anchors")) {
        if (anchors->kind != Kind::Array) scenario_fail("anchors must be an array");
        for (const Value& element : anchors->elements) {
            if (element.kind != Kind::Object) scenario_fail("anchors must be objects");
            citations::CitationAnchor anchor;
            anchor.start_index = required_int(element, "startIndex");
            anchor.end_index = required_int(element, "endIndex");
            anchor.cited_text = required_string(element, "citedText");
            record.anchors.push_back(std::move(anchor));
        }
    }
    return record;
}

// --------------------------------------------------------------------------- trace rendering

std::string render_anchor(const citations::CitationAnchor& anchor) {
    std::string out = "{\"startIndex\":" + std::to_string(anchor.start_index) +
                      ",\"endIndex\":" + std::to_string(anchor.end_index) + ",\"citedText\":";
    append_quoted(out, anchor.cited_text);
    out.push_back('}');
    return out;
}

std::string render_record(const citations::CitationRecord& record) {
    std::string out = "{\"version\":" + std::to_string(record.version) + ",\"sourceId\":";
    append_quoted(out, record.source_id);
    out += ",\"provider\":";
    append_quoted(out, record.provider);
    out += ",\"kind\":";
    append_quoted(out, record.kind);
    out += ",\"title\":";
    append_quoted(out, record.title);
    out += ",\"url\":";
    if (record.url.has_value()) {
        append_quoted(out, *record.url);
    } else {
        out += "null";
    }
    out += ",\"fileName\":";
    if (record.file_name.has_value()) {
        append_quoted(out, *record.file_name);
    } else {
        out += "null";
    }
    out += ",\"location\":";
    if (record.location.has_value()) {
        append_quoted(out, *record.location);
    } else {
        out += "null";
    }
    out += ",\"providerSourceId\":";
    if (record.provider_source_id.has_value()) {
        append_quoted(out, *record.provider_source_id);
    } else {
        out += "null";
    }
    out += ",\"excerpt\":";
    if (record.excerpt.has_value()) {
        append_quoted(out, *record.excerpt);
    } else {
        out += "null";
    }
    out += ",\"anchors\":[";
    for (std::size_t i = 0; i < record.anchors.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += render_anchor(record.anchors[i]);
    }
    out += "]}";
    return out;
}

std::string render_records(const std::vector<citations::CitationRecord>& records) {
    std::string out = "[";
    for (std::size_t i = 0; i < records.size(); ++i) {
        if (i != 0) out.push_back(',');
        out += render_record(records[i]);
    }
    out.push_back(']');
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const json::ParseResult parsed = json::parse(read_file(path));
    if (!parsed.value || parsed.value->kind != Kind::Object) {
        scenario_fail(path.stem().string() + ": scenario must be an object");
    }
    const Value& root = *parsed.value;
    const std::string op = required_string(root, "op");

    if (op == "gpt") {
        const Value* raw_text = field(root, "rawText");
        if (raw_text == nullptr || raw_text->kind != Kind::String) {
            scenario_fail("rawText must be a string");
        }
        const Value* references = field(root, "references");
        if (references == nullptr || references->kind != Kind::Array) {
            scenario_fail("references must be an array");
        }
        const projection::ProjectedText projected =
            projection::project_chatgpt_citations(raw_text->text, references->elements);
        std::string out = "{\"text\":";
        append_quoted(out, projected.text);
        out += ",\"citations\":";
        out += render_records(projected.citations);
        out.push_back('}');
        return out;
    }

    if (op == "claude") {
        const Value* citation = field(root, "citation");
        if (citation == nullptr) scenario_fail("citation is required");
        const int start_index = field(root, "startIndex") != nullptr
                                    ? required_int(root, "startIndex")
                                    : 0;
        const Value* answer_text = field(root, "answerText");
        const std::string answer =
            answer_text != nullptr && answer_text->kind == Kind::String ? answer_text->text : "";
        const std::optional<citations::CitationRecord> record =
            projection::project_claude_citation(*citation, start_index, answer);
        std::string out = "{\"citation\":";
        if (record.has_value()) {
            out += render_record(*record);
        } else {
            out += "null";
        }
        out.push_back('}');
        return out;
    }

    if (op == "encode") {
        const Value* list = field(root, "citations");
        if (list == nullptr || list->kind != Kind::Array) scenario_fail("citations required");
        std::vector<citations::CitationRecord> records;
        for (const Value& element : list->elements) records.push_back(decode_record(element));
        const Value* answer_text = field(root, "answerText");
        const std::string answer =
            answer_text != nullptr && answer_text->kind == Kind::String ? answer_text->text : "";
        const std::optional<std::string> encoded =
            projection::encode_imported_citations(records, answer);
        std::string out = "{\"encoded\":";
        if (encoded.has_value()) {
            append_quoted(out, *encoded);
        } else {
            out += "null";
        }
        out.push_back('}');
        return out;
    }

    scenario_fail(path.stem().string() + ": unknown op " + op);
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

TEST(imported_citation_projection_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_IMPORTED_CITATION_DIR;
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
