// Golden parity test for the C++ Gemini stream read loop.
//
// Replays every transcript under core/tools/golden/gemini-stream/scenarios through the C++ router
// (the response decode, GeminiStreamRouter, the hosted-tool projections, the citation projection
// it emits through) and requires the produced trace to equal the frozen Kotlin trace byte for
// byte. The transcript ops, trace format, escaping, uuid collapsing and termination summary
// mirror the reference driver (core/tools/golden/gemini-stream/GeminiStreamTraceDump.kt) exactly.
#include <algorithm>
#include <cstdint>
#include <cstdio>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/gemini_stream.hpp"
#include "agora/core/json.hpp"
#include "agora/core/stream_normalization.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace gemini_stream = agora::core::gemini_stream;
namespace sn = agora::core::stream_normalization;
namespace citations = agora::core::citations;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** The driver's norm(): dashed AND compact uuid forms collapse to <uuid> (malformed stand-in ids
 *  carry the compact form), mirroring the anthropic-stream stand. */
std::string normalize_uuids(std::string_view value) {
    auto is_hex = [](char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); };
    std::string out;
    std::size_t index = 0;
    while (index < value.size()) {
        if (index + 36 <= value.size()) {
            bool ok = true;
            for (std::size_t offset = 0; offset < 36 && ok; ++offset) {
                const char c = value[index + offset];
                if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
                    ok = c == '-';
                } else {
                    ok = is_hex(c);
                }
            }
            if (ok) {
                out += "<uuid>";
                index += 36;
                continue;
            }
        }
        if (index + 32 <= value.size()) {
            bool ok = true;
            for (std::size_t offset = 0; offset < 32 && ok; ++offset) {
                ok = is_hex(value[index + offset]);
            }
            if (ok) {
                out += "<uuid>";
                index += 32;
                continue;
            }
        }
        out.push_back(value[index]);
        ++index;
    }
    return out;
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

std::string flag(bool value) { return value ? "1" : "0"; }

std::string or_null(const std::optional<std::string>& value) {
    return value ? esc(*value) : "-";
}

// -- canonical record / error / usage rendering ---------------------------------------------------

std::string anchor_field(const citations::CitationAnchor& anchor) {
    return "{" + std::to_string(anchor.start_index) + "," + std::to_string(anchor.end_index) +
           "," + esc(anchor.cited_text) + "}";
}

std::string record_fields(const citations::CitationRecord& record) {
    std::string out;
    out += "v=" + std::to_string(record.version);
    out += ";sourceId=" + esc(record.source_id);
    out += ";provider=" + esc(record.provider);
    out += ";kind=" + esc(record.kind);
    out += ";title=" + esc(record.title);
    out += ";url=" + (record.url ? esc(*record.url) : "null");
    out += ";fileName=" + (record.file_name ? esc(*record.file_name) : "null");
    out += ";location=" + (record.location ? esc(*record.location) : "null");
    out += ";providerSourceId=" + (record.provider_source_id ? esc(*record.provider_source_id) : "null");
    out += ";excerpt=" + (record.excerpt ? esc(*record.excerpt) : "null");
    out += ";anchors=[";
    for (std::size_t i = 0; i < record.anchors.size(); ++i) {
        if (i != 0) out += ',';
        out += anchor_field(record.anchors[i]);
    }
    out += ']';
    return out;
}

std::string error_line(const std::optional<sn::GenerationError>& error) {
    if (!error) return "none";
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sn::ErrorApi>) {
                return "api|code=" + or_null(value.code) + "|type=" + or_null(value.type) +
                       "|message=" + esc(value.message);
            } else if constexpr (std::is_same_v<T, sn::ErrorSseParse>) {
                return "sseParse|raw=" + esc(value.raw_line);
            } else {
                return "unexpectedError";
            }
        },
        *error);
}

std::string usage_line(const gemini_stream::TokenUsage& usage) {
    const auto number = [](const std::optional<std::int32_t>& value) {
        return value ? std::to_string(*value) : std::string("-");
    };
    return "usage|total=" + std::to_string(usage.total_token_count) +
           "|input=" + number(usage.input_token_count) +
           "|cached=" + number(usage.cached_input_token_count) +
           "|cacheWrite=-" +
           "|uncached=" + number(usage.uncached_input_token_count) +
           "|output=" + number(usage.output_token_count) +
           "|reasoning=" + number(usage.reasoning_token_count);
}

std::string event_line(const gemini_stream::StreamEvent& event) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, sn::TextChunk>) {
                return "text|" + esc(value.text);
            } else if constexpr (std::is_same_v<T, sn::ThoughtChunk>) {
                return "thought|" + esc(value.thought) + "|title=" + or_null(value.title) +
                       "|signature=" + or_null(value.signature);
            } else if constexpr (std::is_same_v<T, sn::ToolCallUpdate>) {
                return "toolCallUpdate|streamKey=" + normalize_uuids(value.stream_key) +
                       "|id=" + (value.id ? normalize_uuids(*value.id) : "-") +
                       "|name=" + esc(value.name) + "|args=" + esc(value.arguments) +
                       "|signature=" + or_null(value.signature);
            } else if constexpr (std::is_same_v<T, sn::ToolCallRequest>) {
                return "toolCall|id=" + normalize_uuids(value.id) + "|name=" + esc(value.name) +
                       "|args=" + esc(value.arguments) +
                       "|streamKey=" + normalize_uuids(value.stream_key) +
                       "|signature=" + or_null(value.signature);
            } else if constexpr (std::is_same_v<T, sn::HostedToolCallUpdate>) {
                return "hosted|streamKey=" + normalize_uuids(value.stream_key) +
                       "|name=" + esc(value.name) + "|arguments=" + esc(value.arguments) +
                       "|result=" + or_null(value.result) +
                       "|error=" + flag(value.is_error);
            } else if constexpr (std::is_same_v<T, gemini_stream::CitationUpdate>) {
                return "citation|" + record_fields(value.citation);
            } else if constexpr (std::is_same_v<T, gemini_stream::UsageUpdate>) {
                return usage_line(value.usage);
            } else {
                return "unexpectedEvent";
            }
        },
        event);
}

/** Deterministic dashed uuid for the injectable generator (the traces collapse them anyway). */
std::string deterministic_uuid(int& counter) {
    char buffer[64];
    std::snprintf(buffer, sizeof buffer, "00000000-0000-4000-8000-%012d", counter++);
    return buffer;
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

std::string run_scenario(const std::filesystem::path& scenario_path) {
    const std::string stem = scenario_path.stem().string();
    const std::string source = read_file(scenario_path);
    const json::ParseResult parsed = json::parse(source);
    if (!parsed.value || parsed.value->kind != json::Kind::Object) {
        scenario_fail(stem, "scenario is not a JSON object");
    }
    const json::Value& scenario = *parsed.value;

    const json::Value* name_value = field(scenario, "name");
    const std::string name =
        name_value && name_value->kind == json::Kind::String ? name_value->text : stem;
    if (name != stem) {
        scenario_fail(stem, "scenario name '" + name + "' must match the file stem");
    }
    const json::Value* provider_value = field(scenario, "providerName");
    const std::string provider_name =
        provider_value && provider_value->kind == json::Kind::String
            ? provider_value->text
            : "Google";
    const json::Value* ops_value = field(scenario, "ops");
    if (ops_value == nullptr || ops_value->kind != json::Kind::Array) {
        scenario_fail(stem, "scenario ops must be an array");
    }

    std::string out;
    const auto line = [&](std::initializer_list<std::string> fields) {
        bool first = true;
        for (const std::string& field_value : fields) {
            if (!first) out += '|';
            first = false;
            out += field_value;
        }
        out += '\n';
    };
    line({"name", esc(name)});
    line({"mode", "stream"});

    int uuid_counter = 0;
    gemini_stream::GeminiStreamRouter router(
        [&uuid_counter] { return deterministic_uuid(uuid_counter); }, provider_name);
    bool produced_content = false;
    const auto emit_tracked = [&](const gemini_stream::StreamEvent& event) {
        if (gemini_stream::carries_model_output(event)) produced_content = true;
        out += event_line(event);
        out += '\n';
    };

    bool stopped = false;
    for (const json::Value& op_value : ops_value->elements) {
        if (op_value.kind != json::Kind::String) scenario_fail(stem, "ops must be strings");
        if (stopped) continue;
        const std::string& op = op_value.text;
        if (op == "E") {
            stopped = true;  // readLine returned null: the loop exits at its condition
            continue;
        }
        if (!op.starts_with("L ")) scenario_fail(stem, "unknown op: " + op);
        const std::string line_text = op.substr(2);
        if (!line_text.starts_with("data: ")) continue;
        const std::string json_str = json::trim(std::string_view(line_text).substr(6));
        if (json_str == "[DONE]") {
            router.saw_done = true;
            stopped = true;
            continue;
        }
        gemini_stream::DecodeFailure failure;
        const std::optional<gemini_stream::ApiStreamResponse> response =
            gemini_stream::decode_api_stream_response(json_str, &failure);
        if (!response) {
            router.capture_parse_error(
                json_str, failure.message.empty() ? "Malformed SSE payload" : failure.message);
            stopped = true;
            continue;
        }
        for (const gemini_stream::StreamEvent& event : router.route(*response)) {
            emit_tracked(event);
        }
        if (router.stream_error || router.finish_reason) stopped = true;
    }

    // ---- after the loop: termination decides about the final citation pass --------------------
    sn::StreamTermination termination;
    termination.saw_terminal_marker = router.saw_done || router.finish_reason.has_value();
    termination.stop_reason = router.finish_reason;
    termination.produced_content = produced_content;
    termination.tool_call_in_flight = router.tool_call_in_flight();
    termination.stream_error = router.stream_error;
    if (!termination.is_retryable()) {
        for (const citations::CitationRecord& record : router.final_citations()) {
            emit_tracked(gemini_stream::CitationUpdate{record});
        }
    }

    line({"term", "sawDone=" + flag(termination.saw_terminal_marker),
          "finish=" + or_null(termination.stop_reason), "produced=" + flag(termination.produced_content),
          "inFlight=" + flag(termination.tool_call_in_flight)});
    line({"streamError", error_line(termination.stream_error)});
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

TEST(gemini_stream_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_GEMINI_STREAM_DIR;
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
