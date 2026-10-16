// Golden parity test for the native OpenAI chat SSE consumer.
//
// Replays every transcript under core/tools/golden/openai/transcripts through the C++ consumer
// and requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace
// format, escaping and UUID normalization mirror the reference driver
// (core/tools/golden/openai/OpenAiStreamTraceDump.kt) exactly.
#include <algorithm>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <sstream>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_stream.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;

using agora::core::openai::ChatStreamConsumer;
using agora::core::openai::Error;
using agora::core::openai::Event;
using agora::core::openai::TokenUsage;
using agora::core::openai::ToolCall;
using agora::core::openai::Termination;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

/** BufferedReader.readLine semantics: \n, \r\n and lone \r end a line; a trailing
 *  newline does not add an empty final line. */
std::vector<std::string> read_lines(std::string_view text) {
    std::vector<std::string> lines;
    std::string current;
    for (std::size_t i = 0; i < text.size(); ++i) {
        const char c = text[i];
        if (c == '\n') {
            lines.push_back(current);
            current.clear();
        } else if (c == '\r') {
            lines.push_back(current);
            current.clear();
            if (i + 1 < text.size() && text[i + 1] == '\n') ++i;
        } else {
            current.push_back(c);
        }
    }
    if (!current.empty()) lines.push_back(current);
    return lines;
}

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/** Collapses UUIDs (random per run) to a marker, like the reference driver's regex. */
std::string normalize_uuids(std::string_view value) {
    std::string out;
    std::size_t i = 0;
    while (i < value.size()) {
        bool matched = false;
        if (i + 36 <= value.size()) {
            bool ok = true;
            for (std::size_t k = 0; k < 36 && ok; ++k) {
                const char c = value[i + k];
                if (k == 8 || k == 13 || k == 18 || k == 23) {
                    ok = c == '-';
                } else {
                    ok = is_hex_digit(c);
                }
            }
            if (ok) {
                out += "<uuid>";
                i += 36;
                matched = true;
            }
        }
        if (!matched && i + 32 <= value.size()) {
            bool ok = true;
            for (std::size_t k = 0; k < 32 && ok; ++k) ok = is_hex_digit(value[i + k]);
            if (ok) {
                out += "<uuid>";
                i += 32;
                matched = true;
            }
        }
        if (!matched) {
            out.push_back(value[i]);
            ++i;
        }
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

std::string or_null(const std::optional<std::string>& value) {
    return value ? esc(*value) : std::string("-");
}

std::string or_null_int(const std::optional<std::int32_t>& value) {
    return value ? std::to_string(*value) : std::string("-");
}

std::string flag(bool value) { return value ? "1" : "0"; }

std::string call_line(const ToolCall& call) {
    return "id=" + normalize_uuids(call.id) + "|name=" + esc(call.name) +
           "|args=" + esc(call.arguments) + "|streamKey=" + normalize_uuids(call.stream_key);
}

std::string error_line(const Error& error) {
    switch (error.kind) {
        case Error::Kind::None:
            return "none";
        case Error::Kind::Network:
            return "network|code=" + std::to_string(error.status_code) +
                   "|message=" + esc(error.message);
        case Error::Kind::Api:
            return "api|code=" + or_null(error.code) + "|type=" + or_null(error.type) +
                   "|message=" + esc(error.message);
        case Error::Kind::SseParse:
            return "sseParse|raw=" + esc(error.raw_line);
        case Error::Kind::IncompleteStream:
            return "incomplete|provider=" + esc(error.provider) + "|stop=" +
                   or_null(error.stop_reason) + "|inFlight=" + flag(error.tool_call_in_flight) +
                   "|produced=" + flag(error.produced_content);
        case Error::Kind::OutputTruncated:
            return "outputTruncated|provider=" + esc(error.provider) + "|stop=" +
                   or_null(error.stop_reason);
        case Error::Kind::MalformedToolCall:
            return "malformedToolCall|cause=" + esc(error.cause);
        case Error::Kind::Timeout:
            return "Timeout";
    }
    return "none";
}

std::string usage_line(const TokenUsage& usage) {
    return "usage|total=" + std::to_string(usage.total_token_count) +
           "|input=" + or_null_int(usage.input_token_count) +
           "|cached=" + or_null_int(usage.cached_input_token_count) +
           "|cacheWrite=" + or_null_int(usage.cache_write_input_token_count) +
           "|uncached=" + or_null_int(usage.uncached_input_token_count) +
           "|output=" + or_null_int(usage.output_token_count) +
           "|reasoning=" + or_null_int(usage.reasoning_token_count);
}

std::string event_line(const Event& event) {
    switch (event.kind) {
        case Event::Kind::TextChunk:
            return "text|" + esc(event.text);
        case Event::Kind::ThoughtChunk:
            return "thought|" + esc(event.text) + "|title=" + or_null(event.title) +
                   "|signature=" + or_null(event.signature);
        case Event::Kind::ToolCallUpdate:
            return "toolCallUpdate|streamKey=" + normalize_uuids(event.stream_key) + "|id=" +
                   (event.call_id ? normalize_uuids(*event.call_id) : std::string("-")) +
                   "|name=" + esc(event.name) + "|args=" + esc(event.arguments);
        case Event::Kind::ToolCallRequest: {
            ToolCall call;
            call.id = *event.call_id;
            call.name = event.name;
            call.arguments = event.arguments;
            call.stream_key = event.stream_key;
            return "toolCall|" + call_line(call);
        }
        case Event::Kind::ToolCallsRequest: {
            std::string out = "toolCalls|";
            for (std::size_t i = 0; i < event.calls.size(); ++i) {
                if (i != 0) out += ";;";
                out += call_line(event.calls[i]);
            }
            return out;
        }
        case Event::Kind::UsageUpdate:
            return usage_line(event.usage);
        case Event::Kind::Error:
            return "error|" + error_line(event.error);
    }
    return "none";
}

std::string replay_trace(std::string_view transcript) {
    std::string out;
    ChatStreamConsumer::Options options;
    options.log_malformed = [&out](const std::string& message) {
        out += "log|" + esc(message) + "\n";
    };
    ChatStreamConsumer consumer(std::move(options));
    bool stopped = false;
    bool terminal_active = false;
    for (const std::string& op : read_lines(transcript)) {
        std::string line = op;
        while (!line.empty() && line.back() == '\r') line.pop_back();
        if (json::is_blank(line) || line.rfind("#", 0) == 0) continue;
        if (stopped) break;
        if (line == "T") {
            if (!consumer.on_read_timeout(terminal_active)) stopped = true;
        } else if (line == "E") {
            stopped = true;
        } else if (line.rfind("L ", 0) == 0) {
            auto outcome = consumer.consume_line(std::string_view(line).substr(2));
            for (const Event& event : outcome.events) out += event_line(event) + "\n";
            if (!outcome.keep_reading) {
                stopped = true;
            } else if (!terminal_active && consumer.has_semantic_terminal()) {
                terminal_active = true;
            }
        } else if (line.rfind("R ", 0) == 0) {
            Error error;
            error.kind = Error::Kind::Network;
            error.status_code = 0;
            error.message = line.substr(2);
            consumer.on_read_error(std::move(error));
            stopped = true;
        } else {
            throw agora::core::test::Failure("unknown transcript op: " + line);
        }
    }
    const Termination termination = consumer.finish();
    out += "term|sawTerminal=" + flag(termination.saw_terminal_marker) +
           "|stop=" + or_null(termination.stop_reason) +
           "|produced=" + flag(termination.produced_content) +
           "|inFlight=" + flag(termination.tool_call_in_flight) +
           "|timedOut=" + flag(termination.timed_out) +
           "|reported=" + flag(termination.already_reported_error) +
           "|retryable=" + flag(termination.is_retryable()) +
           "|truncCap=" + flag(termination.truncated_by_token_cap()) + "\n";
    out += "streamError|" + error_line(termination.stream_error) + "\n";
    out += "toError|" + error_line(termination.to_error("agora")) + "\n";
    out += "describe|" + esc(termination.describe()) + "\n";
    return out;
}

std::string describe_mismatch(std::string_view actual, std::string_view expected) {
    const std::size_t common = std::min(actual.size(), expected.size());
    std::size_t at = 0;
    while (at < common && actual[at] == expected[at]) ++at;
    const std::size_t from = at > 24 ? at - 24 : 0;
    const auto window = [](std::string_view text, std::size_t from) {
        const std::size_t to = std::min(text.size(), from + 48);
        std::string out;
        for (std::size_t i = from; i < to; ++i) {
            const unsigned char c = static_cast<unsigned char>(text[i]);
            if (c >= 0x20 && c < 0x7F) {
                out.push_back(static_cast<char>(c));
            } else {
                static constexpr char kHex[] = "0123456789abcdef";
                out += "\\x";
                out.push_back(kHex[c >> 4]);
                out.push_back(kHex[c & 0xF]);
            }
        }
        return out;
    };
    std::ostringstream message;
    message << "trace differs at byte " << at << " (actual " << actual.size() << " bytes, expected "
            << expected.size() << " bytes)\n       actual:   " << window(actual, from)
            << "\n       expected: " << window(expected, from);
    return message.str();
}

}  // namespace

TEST(openai_stream_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_OPENAI_GOLDEN_DIR;
    const std::filesystem::path transcripts_dir = root / "transcripts";
    const std::filesystem::path golden_dir = root / "golden";
    CHECK(std::filesystem::is_directory(transcripts_dir));

    std::vector<std::filesystem::path> transcripts;
    for (const auto& entry : std::filesystem::directory_iterator(transcripts_dir)) {
        if (entry.path().extension() == ".txt") transcripts.push_back(entry.path());
    }
    std::sort(transcripts.begin(), transcripts.end());
    CHECK(!transcripts.empty());

    int checked = 0;
    for (const std::filesystem::path& transcript : transcripts) {
        const std::string expected = read_file(golden_dir / (transcript.stem().string() + ".trace"));
        CHECK(!expected.empty());
        const std::string actual = replay_trace(read_file(transcript));
        if (actual != expected) {
            throw agora::core::test::Failure(transcript.filename().string() + ": " +
                                             describe_mismatch(actual, expected));
        }
        checked++;
    }
    CHECK(checked == static_cast<int>(transcripts.size()));
}
