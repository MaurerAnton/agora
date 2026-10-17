// Golden parity test for the C++ citation pipeline.
//
// Replays every scenario under core/tools/golden/citations/scenarios through the C++ port
// (CitationPolicy, the JDK-26 java.net.URI port, the Anthropic and Gemini citation projections)
// and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format, escaping and mode handling mirror the reference driver
// (core/tools/golden/citations/CitationsTraceDump.kt) exactly, including the outer catch that
// renders an exception class name instead of the mode lines.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/java_uri.hpp"
#include "agora/core/json.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace citations = agora::core::citations;
namespace java_uri = agora::core::java_uri;

using citations::CitationAnchor;
using citations::CitationRecord;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** Field-safe escaping shared with the reference driver. */
std::string esc(std::string_view value) {
    std::string out;
    for (char c : value) {
        switch (c) {
            case '\\': out += "\\\\"; break;
            case '|': out += "\\p"; break;
            case ';': out += "\\s"; break;
            case '"': out += "\\q"; break;
            case '\n': out += "\\n"; break;
            case '\r': out += "\\r"; break;
            case '\t': out += "\\t"; break;
            default: out.push_back(c);
        }
    }
    return out;
}

std::string q(std::string_view value) { return "\"" + esc(value) + "\""; }

std::string qn(const std::optional<std::string>& value) { return value ? q(*value) : "null"; }

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    for (const json::Member& member : object.members) {
        if (member.key == name) return &member.value;
    }
    return nullptr;
}

bool is_json_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

struct ScenarioAnchor {
    std::int32_t start_index = 0;
    std::int32_t end_index = 0;
    std::string cited_text;
};

struct ScenarioRecord {
    std::int32_t version = 1;
    std::string source_id;
    std::string provider;
    std::string kind;
    std::string title;
    std::optional<std::string> url;
    std::optional<std::string> file_name;
    std::optional<std::string> location;
    std::optional<std::string> provider_source_id;
    std::optional<std::string> excerpt;
    std::vector<ScenarioAnchor> anchors;
};

struct Scenario {
    std::string name;
    std::string mode;
    std::string provider;
    std::string kind;
    std::optional<std::string> title;
    std::optional<std::string> url;
    std::optional<std::string> file_name;
    std::optional<std::string> location;
    std::optional<std::string> provider_source_id;
    std::optional<std::string> excerpt;
    std::vector<ScenarioAnchor> anchors;
    std::optional<std::string> answer_text;
    std::int32_t version = 1;
    std::string source_id;
    std::optional<ScenarioRecord> incoming;
    std::vector<ScenarioRecord> records;
    std::string raw;
    std::string query;
    std::string text;
    std::int32_t answer_start_index = 0;
    std::string citation;
    std::string metadata;
    std::vector<std::string> cases;
};

struct Decoder {
    const std::string& scenario;

    std::optional<std::string> optional_string(const json::Value& object,
                                               std::string_view name) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return std::nullopt;
        if (value->kind != json::Kind::String) {
            scenario_fail(scenario, std::string(name) + ": expected a JSON string");
        }
        return value->text;
    }

    std::string string_or(const json::Value& object, std::string_view name,
                          const std::string& fallback) const {
        const auto value = optional_string(object, name);
        return value ? *value : fallback;
    }

    std::int32_t int_or(const json::Value& object, std::string_view name,
                        std::int32_t fallback) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return fallback;
        if (value->kind != json::Kind::Literal) {
            scenario_fail(scenario, std::string(name) + ": expected an Int");
        }
        std::int32_t parsed = 0;
        const auto result =
            std::from_chars(value->text.data(), value->text.data() + value->text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
            scenario_fail(scenario, std::string(name) + ": expected an Int");
        }
        return parsed;
    }

    std::vector<ScenarioAnchor> anchors(const json::Value& object, std::string_view name) const {
        std::vector<ScenarioAnchor> out;
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return out;
        if (value->kind != json::Kind::Array) {
            scenario_fail(scenario, std::string(name) + ": expected an array");
        }
        for (const json::Value& entry : value->elements) {
            if (entry.kind != json::Kind::Object) {
                scenario_fail(scenario, std::string(name) + ": expected objects");
            }
            ScenarioAnchor anchor;
            anchor.start_index = int_or(entry, "startIndex", 0);
            anchor.end_index = int_or(entry, "endIndex", 0);
            anchor.cited_text = string_or(entry, "citedText", "");
            out.push_back(std::move(anchor));
        }
        return out;
    }

    ScenarioRecord record(const json::Value& value) const {
        if (value.kind != json::Kind::Object) scenario_fail(scenario, "record must be an object");
        ScenarioRecord out;
        out.version = int_or(value, "version", 1);
        out.source_id = string_or(value, "sourceId", "");
        out.provider = string_or(value, "provider", "");
        out.kind = string_or(value, "kind", "");
        out.title = string_or(value, "title", "");
        out.url = optional_string(value, "url");
        out.file_name = optional_string(value, "fileName");
        out.location = optional_string(value, "location");
        out.provider_source_id = optional_string(value, "providerSourceId");
        out.excerpt = optional_string(value, "excerpt");
        out.anchors = anchors(value, "anchors");
        return out;
    }

    std::optional<ScenarioRecord> optional_record(const json::Value& object,
                                                  std::string_view name) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return std::nullopt;
        return record(*value);
    }

    std::vector<ScenarioRecord> record_list(const json::Value& object,
                                            std::string_view name) const {
        std::vector<ScenarioRecord> out;
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return out;
        if (value->kind != json::Kind::Array) {
            scenario_fail(scenario, std::string(name) + ": expected an array");
        }
        for (const json::Value& entry : value->elements) out.push_back(record(entry));
        return out;
    }

    std::vector<std::string> string_list(const json::Value& object, std::string_view name) const {
        std::vector<std::string> out;
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return out;
        if (value->kind != json::Kind::Array) {
            scenario_fail(scenario, std::string(name) + ": expected an array");
        }
        for (const json::Value& entry : value->elements) {
            if (entry.kind != json::Kind::String) {
                scenario_fail(scenario, std::string(name) + ": expected strings");
            }
            out.push_back(entry.text);
        }
        return out;
    }

    Scenario decode(const json::Value& root) const {
        if (root.kind != json::Kind::Object) scenario_fail(scenario, "scenario is not an object");
        Scenario out;
        out.name = string_or(root, "name", "");
        out.mode = string_or(root, "mode", "");
        out.provider = string_or(root, "provider", "");
        out.kind = string_or(root, "kind", "");
        out.title = optional_string(root, "title");
        out.url = optional_string(root, "url");
        out.file_name = optional_string(root, "fileName");
        out.location = optional_string(root, "location");
        out.provider_source_id = optional_string(root, "providerSourceId");
        out.excerpt = optional_string(root, "excerpt");
        out.anchors = anchors(root, "anchors");
        out.answer_text = optional_string(root, "answerText");
        out.version = int_or(root, "version", 1);
        out.source_id = string_or(root, "sourceId", "");
        out.incoming = optional_record(root, "incoming");
        out.records = record_list(root, "records");
        out.raw = string_or(root, "raw", "");
        out.query = string_or(root, "query", "");
        out.text = string_or(root, "text", "");
        out.answer_start_index = int_or(root, "answerStartIndex", 0);
        out.citation = string_or(root, "citation", "");
        out.metadata = string_or(root, "metadata", "");
        out.cases = string_list(root, "cases");
        return out;
    }
};

CitationAnchor to_model(const ScenarioAnchor& anchor) {
    CitationAnchor out;
    out.start_index = anchor.start_index;
    out.end_index = anchor.end_index;
    out.cited_text = anchor.cited_text;
    return out;
}

CitationRecord to_model(const ScenarioRecord& record) {
    CitationRecord out;
    out.version = record.version;
    out.source_id = record.source_id;
    out.provider = record.provider;
    out.kind = record.kind;
    out.title = record.title;
    out.url = record.url;
    out.file_name = record.file_name;
    out.location = record.location;
    out.provider_source_id = record.provider_source_id;
    out.excerpt = record.excerpt;
    for (const ScenarioAnchor& anchor : record.anchors) out.anchors.push_back(to_model(anchor));
    return out;
}

/** Scenario.toModel(): the record built from the top-level scenario fields. */
CitationRecord top_level_record(const Scenario& scenario) {
    CitationRecord out;
    out.version = scenario.version;
    out.source_id = scenario.source_id;
    out.provider = scenario.provider;
    out.kind = scenario.kind;
    out.title = scenario.title.value_or("");
    out.url = scenario.url;
    out.file_name = scenario.file_name;
    out.location = scenario.location;
    out.provider_source_id = scenario.provider_source_id;
    out.excerpt = scenario.excerpt;
    for (const ScenarioAnchor& anchor : scenario.anchors) out.anchors.push_back(to_model(anchor));
    return out;
}

std::string anchor_field(const CitationAnchor& anchor) {
    return "{" + std::to_string(anchor.start_index) + "," + std::to_string(anchor.end_index) +
           "," + q(anchor.cited_text) + "}";
}

std::string record_fields(const CitationRecord& record) {
    std::string out;
    out += "v=" + std::to_string(record.version);
    out += ";sourceId=" + q(record.source_id);
    out += ";provider=" + q(record.provider);
    out += ";kind=" + q(record.kind);
    out += ";title=" + q(record.title);
    out += ";url=" + qn(record.url);
    out += ";fileName=" + qn(record.file_name);
    out += ";location=" + qn(record.location);
    out += ";providerSourceId=" + qn(record.provider_source_id);
    out += ";excerpt=" + qn(record.excerpt);
    out += ";anchors=[";
    for (std::size_t i = 0; i < record.anchors.size(); ++i) {
        if (i != 0) out += ',';
        out += anchor_field(record.anchors[i]);
    }
    out += ']';
    return out;
}

std::vector<CitationRecord> to_model(const std::vector<ScenarioRecord>& records) {
    std::vector<CitationRecord> out;
    out.reserve(records.size());
    for (const ScenarioRecord& record : records) out.push_back(to_model(record));
    return out;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::string text = read_file(path);
    const json::ParseResult parsed = json::parse(text);
    if (!parsed.value.has_value()) {
        scenario_fail(path.filename().string(), "scenario JSON does not parse: " + parsed.error);
    }
    const std::string stem = path.stem().string();
    const Scenario scenario = Decoder{stem}.decode(*parsed.value);
    if (scenario.name != stem) {
        scenario_fail(stem, "scenario name '" + scenario.name + "' must match the file stem");
    }

    std::string out;
    const auto line = [&out](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& value : fields) {
            if (!first) out.push_back('|');
            out += value;
            first = false;
        }
        out.push_back('\n');
    };

    line({"name", esc(scenario.name)});
    line({"mode", esc(scenario.mode)});
    try {
        if (scenario.mode == "create") {
            std::vector<CitationAnchor> anchors;
            for (const ScenarioAnchor& anchor : scenario.anchors) {
                anchors.push_back(to_model(anchor));
            }
            const std::optional<CitationRecord> record = citations::policy::create(
                scenario.provider, scenario.kind, scenario.title, scenario.url, scenario.file_name,
                scenario.location, scenario.provider_source_id, scenario.excerpt, anchors,
                scenario.answer_text);
            line({"create", record ? record_fields(*record) : "null"});
        } else if (scenario.mode == "normalize") {
            const std::optional<CitationRecord> record = citations::policy::normalize_record(
                top_level_record(scenario), scenario.answer_text);
            line({"normalize", record ? record_fields(*record) : "null"});
        } else if (scenario.mode == "merge") {
            if (!scenario.incoming) scenario_fail(stem, "merge requires incoming");
            const CitationRecord merged = citations::policy::merge(
                top_level_record(scenario), to_model(*scenario.incoming));
            line({"merge", record_fields(merged)});
        } else if (scenario.mode == "deduplicate") {
            const std::vector<CitationRecord> deduped = citations::policy::deduplicate(
                to_model(scenario.records), scenario.answer_text);
            line({"count", std::to_string(deduped.size())});
            for (std::size_t i = 0; i < deduped.size(); ++i) {
                line({"record", std::to_string(i), record_fields(deduped[i])});
            }
        } else if (scenario.mode == "strip") {
            line({"strip", q(citations::policy::strip_private_markers(scenario.text))});
        } else if (scenario.mode == "copy") {
            line({"copy", q(citations::policy::copy_text(scenario.text, to_model(scenario.records)))});
        } else if (scenario.mode == "encode") {
            line({"encode", q(citations::policy::encode(top_level_record(scenario)))});
        } else if (scenario.mode == "decode") {
            const std::optional<CitationRecord> record =
                citations::policy::decode(scenario.raw, scenario.answer_text);
            line({"decode", record ? record_fields(*record) : "null"});
        } else if (scenario.mode == "safeUrl") {
            line({"safeUrl", qn(citations::policy::safe_http_url(scenario.url))});
        } else if (scenario.mode == "matchTitle") {
            line({"matchTitle",
                  citations::matches_citation_title(to_model(scenario.records), scenario.query) ? "1"
                                                                                               : "0"});
        } else if (scenario.mode == "anthropicCitation") {
            const json::ParseResult element = json::parse(scenario.citation);
            if (!element.value.has_value()) {
                line({"parse", "throw", "JsonDecodingException"});
            } else {
                const std::optional<CitationRecord> record = citations::to_anthropic_citation_record(
                    *element.value, scenario.answer_start_index, scenario.text);
                line({"record", record ? record_fields(*record) : "null"});
            }
        } else if (scenario.mode == "geminiCitations") {
            const json::ParseResult element = json::parse(scenario.metadata);
            if (!element.value.has_value()) {
                line({"parse", "throw", "JsonDecodingException"});
            } else if (element.value->kind != json::Kind::Object) {
                line({"metadata", "notObject"});
            } else {
                const std::vector<CitationRecord> records =
                    citations::to_gemini_citations(*element.value, scenario.text);
                line({"count", std::to_string(records.size())});
                for (std::size_t i = 0; i < records.size(); ++i) {
                    line({"record", std::to_string(i), record_fields(records[i])});
                }
            }
        } else if (scenario.mode == "uri") {
            for (std::size_t i = 0; i < scenario.cases.size(); ++i) {
                const std::string& raw = scenario.cases[i];
                line({"case", std::to_string(i), q(raw)});
                const std::optional<java_uri::Uri> uri = java_uri::parse(raw);
                if (!uri) {
                    line({"parse", "throw", "URISyntaxException"});
                    continue;
                }
                line({"parse",
                      "scheme=" + qn(uri->scheme),
                      "userInfo=" + qn(java_uri::decoded_user_info(*uri)),
                      "host=" + qn(uri->host),
                      "port=" + std::to_string(uri->port),
                      "path=" + qn(java_uri::decoded_path(*uri)),
                      "query=" + qn(java_uri::decoded_query(*uri)),
                      "fragment=" + qn(java_uri::decoded_fragment(*uri)),
                      "authority=" + qn(java_uri::decoded_authority(*uri)),
                      "opaque=" + std::string(java_uri::is_opaque(*uri) ? "1" : "0"),
                      "absolute=" + std::string(java_uri::is_absolute(*uri) ? "1" : "0")});
                line({"ascii", q(java_uri::to_ascii_string(*uri))});
                const java_uri::Uri normalized = java_uri::normalize(*uri);
                line({"normalized", q(java_uri::to_string(normalized))});
                line({"normalizedAscii", q(java_uri::to_ascii_string(normalized))});
            }
        } else if (scenario.mode == "safeUrlFuzz") {
            for (std::size_t i = 0; i < scenario.cases.size(); ++i) {
                const std::string& raw = scenario.cases[i];
                line({"case", std::to_string(i), q(raw),
                      qn(citations::policy::safe_http_url(raw))});
            }
        } else {
            scenario_fail(stem, "unknown mode: " + scenario.mode);
        }
    } catch (const std::invalid_argument&) {
        // Kotlin require() maps to IllegalArgumentException in the reference trace.
        line({"error", "IllegalArgumentException"});
    }
    return out;
}

std::string window(std::string_view text, std::size_t from) {
    const std::size_t begin = from > 40 ? from - 40 : 0;
    const std::size_t end = std::min(text.size(), from + 40);
    return std::string(text.substr(begin, end - begin));
}

std::string describe_mismatch(const std::string& actual, const std::string& expected) {
    std::size_t from = 0;
    while (from < actual.size() && from < expected.size() && actual[from] == expected[from]) ++from;
    std::ostringstream message;
    message << "trace mismatch at byte " << from << " (actual " << actual.size()
            << " bytes, expected " << expected.size() << " bytes)\n       actual:   "
            << window(actual, from) << "\n       expected: " << window(expected, from);
    return message.str();
}

}  // namespace

TEST(citations_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_CITATIONS_DIR;
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
