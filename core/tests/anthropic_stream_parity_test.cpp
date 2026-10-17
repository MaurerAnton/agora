// Golden parity test for the C++ Anthropic stream event router.
//
// Replays every transcript under core/tools/golden/anthropic-stream/scenarios through the C++
// port (the event-model decode walk, AnthropicStreamEventRouter, the citation projection it
// emits through) and requires the produced trace to equal the frozen Kotlin trace byte for byte.
// The transcript ops, trace format, escaping, uuid collapsing and termination summary mirror the
// reference driver (core/tools/golden/anthropic-stream/AnthropicStreamTraceDump.kt) exactly.
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

#include "agora/core/anthropic_stream.hpp"
#include "agora/core/json.hpp"
#include "agora/core/stream_normalization.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace anthropic = agora::core::anthropic_stream;
namespace sn = agora::core::stream_normalization;
namespace citations = agora::core::citations;

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

std::string or_null(const std::optional<std::string>& value) {
    return value ? esc(*value) : "-";
}

std::string or_null_int(const std::optional<std::int32_t>& value) {
    return value ? std::to_string(*value) : "-";
}

std::string flag(bool value) { return value ? "1" : "0"; }

// -- uuid collapsing, mirroring the reference regex ------------------------------------------

bool is_lower_hex(char c) { return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f'); }

bool dashed_uuid_at(std::string_view text, std::size_t at) {
    if (at + 36 > text.size()) return false;
    for (int i = 0; i < 36; ++i) {
        const bool expect_dash = i == 8 || i == 13 || i == 18 || i == 23;
        const char c = text[at + static_cast<std::size_t>(i)];
        if (expect_dash ? c != '-' : !is_lower_hex(c)) return false;
    }
    return true;
}

bool hex32_at(std::string_view text, std::size_t at) {
    if (at + 32 > text.size()) return false;
    for (int i = 0; i < 32; ++i) {
        if (!is_lower_hex(text[at + static_cast<std::size_t>(i)])) return false;
    }
    return true;
}

std::string norm(std::string_view value) {
    std::string out;
    std::size_t i = 0;
    while (i < value.size()) {
        if (dashed_uuid_at(value, i)) {
            out += "<uuid>";
            i += 36;
            continue;
        }
        if (hex32_at(value, i)) {
            out += "<uuid>";
            i += 32;
            continue;
        }
        out.push_back(value[i]);
        ++i;
    }
    return out;
}

// -- canonical record / error / usage rendering ----------------------------------------------

std::string anchor_field(const citations::CitationAnchor& anchor) {
    return "{" + std::to_string(anchor.start_index) + "," + std::to_string(anchor.end_index) +
           "," + q(anchor.cited_text) + "}";
}

std::string record_fields(const citations::CitationRecord& record) {
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

/** Error identity without implementation-specific cause texts (they differ by design). */
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

std::string usage_line(const anthropic::TokenUsage& usage) {
    return "usage|total=" + std::to_string(usage.total_token_count) +
           "|input=" + or_null_int(usage.input_token_count) +
           "|cached=" + or_null_int(usage.cached_input_token_count) +
           "|cacheWrite=" + or_null_int(usage.cache_write_input_token_count) +
           "|uncached=" + or_null_int(usage.uncached_input_token_count) +
           "|output=" + or_null_int(usage.output_token_count) +
           "|reasoning=" + or_null_int(usage.reasoning_token_count);
}

std::string event_line(const anthropic::StreamEvent& event) {
    return std::visit(
        [](const auto& value) -> std::string {
            using T = std::decay_t<decltype(value)>;
            if constexpr (std::is_same_v<T, anthropic::TextChunk>) {
                return "text|" + esc(value.text);
            } else if constexpr (std::is_same_v<T, anthropic::ThoughtChunk>) {
                return "thought|" + esc(value.thought) + "|title=" + or_null(value.title) +
                       "|signature=" + or_null(value.signature);
            } else if constexpr (std::is_same_v<T, anthropic::ToolCallUpdate>) {
                return "toolCallUpdate|streamKey=" + norm(value.stream_key) + "|id=" +
                       (value.id ? norm(*value.id) : "-") + "|name=" + esc(value.name) +
                       "|args=" + esc(value.arguments);
            } else if constexpr (std::is_same_v<T, anthropic::ToolCallRequest>) {
                return "toolCall|id=" + norm(value.id) + "|name=" + esc(value.name) +
                       "|args=" + esc(value.arguments) + "|streamKey=" + norm(value.stream_key);
            } else if constexpr (std::is_same_v<T, anthropic::UsageUpdate>) {
                return usage_line(value.usage);
            } else if constexpr (std::is_same_v<T, anthropic::CitationUpdate>) {
                return "citation|" + record_fields(value.citation);
            } else if constexpr (std::is_same_v<T, anthropic::ErrorEvent>) {
                return "error|" + error_line(value.error);
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

std::vector<std::string> transcript_ops(std::string_view content) {
    std::vector<std::string> ops;
    std::size_t start = 0;
    while (start <= content.size()) {
        std::size_t end = content.find('\n', start);
        if (end == std::string_view::npos) end = content.size();
        std::string line(content.substr(start, end - start));
        if (!line.empty() && line.back() == '\r') line.pop_back();
        if (!json::is_blank(line) && !line.starts_with("#")) ops.push_back(std::move(line));
        if (end == content.size()) break;
        start = end + 1;
    }
    return ops;
}

std::string run_scenario(const std::filesystem::path& path) {
    const std::vector<std::string> ops = transcript_ops(read_file(path));

    std::string out;
    int uuid_counter = 0;
    anthropic::AnthropicStreamEventRouter router(
        [&uuid_counter] { return deterministic_uuid(uuid_counter); });
    bool produced_content = false;
    bool driver_reported = false;
    const auto emit_tracked = [&](const anthropic::StreamEvent& event) {
        if (anthropic::carries_model_output(event)) produced_content = true;
        if (std::holds_alternative<anthropic::ErrorEvent>(event)) driver_reported = true;
        out += event_line(event);
        out += '\n';
    };

    bool stopped = false;
    for (const std::string& op : ops) {
        if (op == "E") {
            stopped = true;
            continue;
        }
        if (op == "I") {
            for (const anthropic::StreamEvent& event : router.report_incomplete_blocks()) {
                emit_tracked(event);
            }
            continue;
        }
        if (op.starts_with("C ")) {
            router.capture_parse_error(op.substr(2), "driver");
            stopped = true;
            continue;
        }
        if (op.starts_with("L ")) {
            if (stopped) continue;
            const std::string line = op.substr(2);
            if (line.starts_with("data: ")) {
                const std::string json_str = json::trim(std::string_view(line).substr(6));
                anthropic::DecodeFailure failure;
                const std::optional<anthropic::AnthropicStreamEvent> event =
                    anthropic::decode_stream_event(json_str, &failure);
                if (!event) {
                    router.capture_parse_error(
                        json_str,
                        failure.message.empty() ? "Malformed SSE payload" : failure.message);
                    stopped = true;
                } else {
                    for (const anthropic::StreamEvent& emitted : router.route(*event)) {
                        emit_tracked(emitted);
                    }
                }
            }
            if (router.stream_error || router.reported_error || driver_reported) stopped = true;
            if (router.message_stop_received) stopped = true;
            continue;
        }
        scenario_fail(path.filename().string(), "unknown op: " + op);
    }

    for (const anthropic::StreamEvent& event : router.report_incomplete_blocks()) {
        emit_tracked(event);
    }
    out += "term|messageStop=" + flag(router.message_stop_received) +
           "|stop=" + or_null(router.stop_reason) +
           "|toolUseStarts=" + std::to_string(router.tool_use_block_starts) +
           "|sawTerminal=" + flag(router.saw_terminal_marker()) +
           "|inFlight=" + flag(router.tool_call_in_flight()) +
           "|reported=" + flag(router.reported_error) +
           "|produced=" + flag(produced_content) +
           "|driverReported=" + flag(driver_reported) + "\n";
    out += "streamError|" + error_line(router.stream_error) + "\n";
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

TEST(anthropic_stream_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_ANTHROPIC_STREAM_DIR;
    const std::filesystem::path scenarios_dir = root / "scenarios";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(scenarios_dir));
    CHECK(std::filesystem::is_directory(golden_dir));

    std::vector<std::filesystem::path> scenarios;
    for (const auto& entry : std::filesystem::directory_iterator(scenarios_dir)) {
        if (entry.path().extension() == ".txt") scenarios.push_back(entry.path());
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
