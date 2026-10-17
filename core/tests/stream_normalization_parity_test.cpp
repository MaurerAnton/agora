// Golden parity test for the C++ stream post-processing pipeline.
//
// Replays every scenario under core/tools/golden/stream-normalization/scenarios through the C++
// port (ProviderStreamNormalizer, IncrementalThinkingParser, the text tool-call parsers,
// StreamTermination, ToolArgumentAccumulator, MalformedToolCall, ProviderRetryPolicy) and
// requires the produced trace to equal the frozen Kotlin trace byte for byte. The trace format,
// escaping, UUID normalization and mode handling mirror the reference driver
// (core/tools/golden/stream-normalization/StreamNormalizationTraceDump.kt) exactly.
#include <algorithm>
#include <charconv>
#include <cstdint>
#include <filesystem>
#include <fstream>
#include <optional>
#include <set>
#include <sstream>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/stream_normalization.hpp"
#include "check.hpp"

namespace {

namespace json = agora::core::json;
namespace sn = agora::core::stream_normalization;

std::string read_file(const std::filesystem::path& path) {
    std::ifstream stream(path, std::ios::binary);
    std::ostringstream buffer;
    buffer << stream.rdbuf();
    return buffer.str();
}

bool is_hex_digit(char c) {
    return (c >= '0' && c <= '9') || (c >= 'a' && c <= 'f');
}

/**
 * The driver's norm(): dashed UUIDs collapse first, then the two compact-suffix families the
 * malformed stand-in produces (prefix-anchored so deterministic hex runs are not swallowed).
 */
std::string normalize_uuids(std::string_view value) {
    std::string dashed_replaced;
    std::size_t index = 0;
    while (index < value.size()) {
        if (index + 36 <= value.size()) {
            bool ok = true;
            for (std::size_t offset = 0; offset < 36 && ok; ++offset) {
                const char c = value[index + offset];
                if (offset == 8 || offset == 13 || offset == 18 || offset == 23) {
                    ok = c == '-';
                } else {
                    ok = is_hex_digit(c);
                }
            }
            if (ok) {
                dashed_replaced += "<uuid>";
                index += 36;
                continue;
            }
        }
        dashed_replaced.push_back(value[index]);
        ++index;
    }

    const auto replace_compact = [](std::string_view text, std::string_view prefix) {
        std::string out;
        std::size_t at = 0;
        while (at < text.size()) {
            if (at + prefix.size() + 32 <= text.size() &&
                text.compare(at, prefix.size(), prefix) == 0) {
                bool ok = true;
                for (std::size_t offset = 0; offset < 32 && ok; ++offset) {
                    ok = is_hex_digit(text[at + prefix.size() + offset]);
                }
                if (ok) {
                    out += prefix;
                    out += "<uuid>";
                    at += prefix.size() + 32;
                    continue;
                }
            }
            out.push_back(text[at]);
            ++at;
        }
        return out;
    };
    return replace_compact(
        replace_compact(dashed_replaced, "call_malformed_"),
        "malformed_tool_");
}

/** Field-safe escaping for the trace format: no raw separators or control characters survive. */
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

/** Deterministic dashed lowercase UUID source; every value normalizes to <uuid> in the trace. */
class DeterministicUuids {
public:
    sn::UuidFn function() {
        return [this]() { return next(); };
    }

private:
    std::string next() {
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out = "00000000-0000-4000-8000-";
        const std::uint64_t value = counter_++;
        for (int shift = 44; shift >= 0; shift -= 4) {
            out.push_back(kHex[(value >> shift) & 0xF]);
        }
        return out;
    }

    std::uint64_t counter_ = 0;
};

// -- scenario decoding (the @Serializable Scenario shape of the reference driver) ---------------

[[noreturn]] void scenario_fail(const std::string& scenario, const std::string& what) {
    throw agora::core::test::Failure(scenario + ": " + what);
}

const json::Value* field(const json::Value& object, std::string_view name) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_json_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

struct ScenarioEvent {
    std::string type;
    std::string text;
    std::string thought;
    std::optional<std::string> title;
    std::optional<std::string> signature;
    std::optional<std::string> id;
    std::string name;
    std::string arguments;
    std::optional<std::string> stream_key;
    std::vector<ScenarioEvent> calls;
    int token_count = 0;
    int attempt = 0;
    int max_attempts = 0;
    bool is_error = false;
    std::optional<std::string> result;
};

struct Scenario {
    std::string name;
    std::string mode = "normalizer";
    std::vector<std::string> tools;
    bool native_text_parsing_authoritative = false;
    bool release_text_tools = true;
    std::vector<ScenarioEvent> events;
    std::vector<std::string> chunks;
    bool start_in_thinking = false;
    bool thinking_enabled = true;
    std::optional<bool> flush_thinking_enabled;
    std::string content;
    std::vector<std::optional<std::string>> fragments;
    std::string text;
    bool saw_terminal_marker = false;
    std::optional<std::string> stop_reason;
    bool produced_content = false;
    bool tool_call_in_flight = false;
    bool has_stream_error = false;
    std::string stream_error_message;
    bool already_reported_error = false;
    bool timed_out = false;
    bool retryable_stream_error = true;
    std::string cause;
    std::optional<std::string> original_name;
    std::optional<std::string> original_arguments;
    std::optional<std::string> stream_key;
    std::optional<std::string> signature;
    int retry_number = 1;
    int status_code = 500;
    std::optional<std::string> body;
    std::vector<int> retryable_status_codes = {429, 500, 503};
};

struct Decoder {
    const std::string& scenario;

    std::optional<std::string> optional_string(const json::Value& object, std::string_view name) const {
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

    bool bool_or(const json::Value& object, std::string_view name, bool fallback) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return fallback;
        if (value->kind != json::Kind::Literal || (value->text != "true" && value->text != "false")) {
            scenario_fail(scenario, std::string(name) + ": expected a JSON boolean");
        }
        return value->text == "true";
    }

    std::optional<bool> optional_bool(const json::Value& object, std::string_view name) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return std::nullopt;
        return bool_or(object, name, false);
    }

    int int_or(const json::Value& object, std::string_view name, int fallback) const {
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return fallback;
        if (value->kind != json::Kind::Literal) {
            scenario_fail(scenario, std::string(name) + ": expected an Int");
        }
        int parsed = 0;
        const auto result =
            std::from_chars(value->text.data(), value->text.data() + value->text.size(), parsed);
        if (result.ec != std::errc{} || result.ptr != value->text.data() + value->text.size()) {
            scenario_fail(scenario, std::string(name) + ": expected an Int");
        }
        return parsed;
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

    std::vector<std::optional<std::string>> optional_string_list(
        const json::Value& object,
        std::string_view name) const {
        std::vector<std::optional<std::string>> out;
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return out;
        if (value->kind != json::Kind::Array) {
            scenario_fail(scenario, std::string(name) + ": expected an array");
        }
        for (const json::Value& entry : value->elements) {
            if (is_json_null(&entry)) {
                out.push_back(std::nullopt);
            } else if (entry.kind == json::Kind::String) {
                out.push_back(entry.text);
            } else {
                scenario_fail(scenario, std::string(name) + ": expected strings or nulls");
            }
        }
        return out;
    }

    std::vector<int> int_list(const json::Value& object, std::string_view name) const {
        std::vector<int> out;
        const json::Value* value = field(object, name);
        if (value == nullptr || is_json_null(value)) return out;
        if (value->kind != json::Kind::Array) {
            scenario_fail(scenario, std::string(name) + ": expected an array");
        }
        for (const json::Value& entry : value->elements) {
            if (entry.kind != json::Kind::Literal) {
                scenario_fail(scenario, std::string(name) + ": expected Ints");
            }
            int parsed = 0;
            const auto result =
                std::from_chars(entry.text.data(), entry.text.data() + entry.text.size(), parsed);
            if (result.ec != std::errc{} || result.ptr != entry.text.data() + entry.text.size()) {
                scenario_fail(scenario, std::string(name) + ": expected Ints");
            }
            out.push_back(parsed);
        }
        return out;
    }

    ScenarioEvent event(const json::Value& value) const {
        if (value.kind != json::Kind::Object) scenario_fail(scenario, "event must be an object");
        ScenarioEvent out;
        out.type = string_or(value, "type", "");
        out.text = string_or(value, "text", "");
        out.thought = string_or(value, "thought", "");
        out.title = optional_string(value, "title");
        out.signature = optional_string(value, "signature");
        out.id = optional_string(value, "id");
        out.name = string_or(value, "name", "");
        out.arguments = string_or(value, "arguments", "");
        out.stream_key = optional_string(value, "streamKey");
        out.token_count = int_or(value, "tokenCount", 0);
        out.attempt = int_or(value, "attempt", 0);
        out.max_attempts = int_or(value, "maxAttempts", 0);
        out.is_error = bool_or(value, "isError", false);
        out.result = optional_string(value, "result");
        const json::Value* calls = field(value, "calls");
        if (calls != nullptr && !is_json_null(calls)) {
            if (calls->kind != json::Kind::Array) scenario_fail(scenario, "calls must be an array");
            for (const json::Value& entry : calls->elements) {
                out.calls.push_back(event(entry));
            }
        }
        return out;
    }

    Scenario decode(const json::Value& root) const {
        if (root.kind != json::Kind::Object) scenario_fail(scenario, "scenario is not an object");
        Scenario out;
        out.name = string_or(root, "name", "");
        out.mode = string_or(root, "mode", "normalizer");
        out.tools = string_list(root, "tools");
        out.native_text_parsing_authoritative = bool_or(root, "nativeTextParsingAuthoritative", false);
        out.release_text_tools = bool_or(root, "releaseTextTools", true);
        const json::Value* events = field(root, "events");
        if (events != nullptr && !is_json_null(events)) {
            if (events->kind != json::Kind::Array) scenario_fail(scenario, "events must be an array");
            for (const json::Value& entry : events->elements) {
                out.events.push_back(event(entry));
            }
        }
        out.chunks = string_list(root, "chunks");
        out.start_in_thinking = bool_or(root, "startInThinking", false);
        out.thinking_enabled = bool_or(root, "thinkingEnabled", true);
        out.flush_thinking_enabled = optional_bool(root, "flushThinkingEnabled");
        out.content = string_or(root, "content", "");
        out.fragments = optional_string_list(root, "fragments");
        out.text = string_or(root, "text", "");
        out.saw_terminal_marker = bool_or(root, "sawTerminalMarker", false);
        out.stop_reason = optional_string(root, "stopReason");
        out.produced_content = bool_or(root, "producedContent", false);
        out.tool_call_in_flight = bool_or(root, "toolCallInFlight", false);
        out.has_stream_error = bool_or(root, "hasStreamError", false);
        out.stream_error_message = string_or(root, "streamErrorMessage", "");
        out.already_reported_error = bool_or(root, "alreadyReportedError", false);
        out.timed_out = bool_or(root, "timedOut", false);
        out.retryable_stream_error = bool_or(root, "retryableStreamError", true);
        out.cause = string_or(root, "cause", "");
        out.original_name = optional_string(root, "originalName");
        out.original_arguments = optional_string(root, "originalArguments");
        out.stream_key = optional_string(root, "streamKey");
        out.signature = optional_string(root, "signature");
        out.retry_number = int_or(root, "retryNumber", 1);
        out.status_code = int_or(root, "statusCode", 500);
        out.body = optional_string(root, "body");
        out.retryable_status_codes = int_list(root, "retryableStatusCodes");
        if (field(root, "retryableStatusCodes") == nullptr) {
            out.retryable_status_codes = {429, 500, 503};
        }
        return out;
    }
};

// -- event building (the driver's buildEvent) ---------------------------------------------------

sn::ToolCallRequest build_tool_call(const ScenarioEvent& event) {
    sn::ToolCallRequest call;
    call.id = event.id.value_or("");
    call.name = event.name;
    call.arguments = event.arguments;
    call.signature = event.signature;
    call.stream_key = event.stream_key.has_value() ? *event.stream_key : event.id.value_or("");
    return call;
}

sn::StreamEvent build_event(const ScenarioEvent& event) {
    if (event.type == "text") return sn::StreamEvent{sn::TextChunk{event.text}};
    if (event.type == "thought") {
        return sn::StreamEvent{sn::ThoughtChunk{event.thought, event.title, event.signature}};
    }
    if (event.type == "toolCall") return sn::StreamEvent{build_tool_call(event)};
    if (event.type == "toolCalls") {
        sn::ToolCallsRequest request;
        for (const ScenarioEvent& child : event.calls) request.calls.push_back(build_tool_call(child));
        return sn::StreamEvent{std::move(request)};
    }
    if (event.type == "error") {
        return sn::StreamEvent{sn::ErrorEvent{sn::ErrorSseParse{"raw_line", "cause"}}};
    }
    if (event.type == "usage") {
        sn::UsageUpdate usage;
        usage.total_token_count = event.token_count > 0 ? event.token_count : 0;
        usage.reasoning_token_count = std::nullopt;
        return sn::StreamEvent{usage};
    }
    if (event.type == "retrying") return sn::StreamEvent{sn::Retrying{event.attempt, event.max_attempts}};
    if (event.type == "toolCallUpdate") {
        sn::ToolCallUpdate update;
        update.stream_key = event.stream_key.value_or("");
        update.id = event.id;
        update.name = event.name;
        update.arguments = event.arguments;
        update.signature = event.signature;
        return sn::StreamEvent{std::move(update)};
    }
    if (event.type == "hosted") {
        sn::HostedToolCallUpdate hosted;
        hosted.stream_key = event.stream_key.value_or("");
        hosted.name = event.name;
        hosted.arguments = event.arguments;
        hosted.result = event.result;
        hosted.is_error = event.is_error;
        return sn::StreamEvent{std::move(hosted)};
    }
    if (event.type == "citation") return sn::StreamEvent{sn::CitationUpdate{}};
    throw agora::core::test::Failure("unknown event type: " + event.type);
}

// -- downstream trace lines (the driver's eventLine / errorLine) --------------------------------

std::string error_line(const sn::GenerationError& error) {
    struct Visitor {
        std::string operator()(const sn::ErrorNetwork& value) const {
            return "network|" + std::to_string(value.status_code) + "|" + esc(value.message);
        }
        std::string operator()(const sn::ErrorApi& value) const {
            return "api|" + esc(value.code.value_or("-")) + "|" + esc(value.type.value_or("-")) + "|" +
                esc(value.message);
        }
        std::string operator()(const sn::ErrorSseParse& value) const {
            return "sseParse|" + esc(value.raw_line) + "|" + esc(value.cause);
        }
        std::string operator()(const sn::ErrorMalformedToolCall& value) const {
            return "malformedToolCall|" + esc(value.cause);
        }
        std::string operator()(const sn::ErrorIncompleteStream& value) const {
            return "incomplete|" + esc(value.provider) + "|" + esc(value.stop_reason.value_or("-")) +
                "|" + (value.tool_call_in_flight ? "1" : "0") + "|" +
                (value.produced_content ? "1" : "0");
        }
        std::string operator()(const sn::ErrorOutputTruncated& value) const {
            return "outputTruncated|" + esc(value.provider) + "|" +
                esc(value.stop_reason.value_or("-"));
        }
        std::string operator()(const sn::ErrorTimeout&) const { return "Timeout"; }
    };
    return std::visit(Visitor{}, error);
}

std::string event_line(const sn::StreamEvent& event) {
    if (const auto* text = std::get_if<sn::TextChunk>(&event)) {
        return "text|" + esc(text->text);
    }
    if (const auto* thought = std::get_if<sn::ThoughtChunk>(&event)) {
        return "thought|" + esc(thought->thought) + "|" + esc(thought->title.value_or("-")) + "|" +
            esc(thought->signature.value_or("-"));
    }
    if (const auto* call = std::get_if<sn::ToolCallRequest>(&event)) {
        return "toolCall|" + esc(normalize_uuids(call->id)) + "|" + esc(call->name) + "|" +
            esc(call->arguments) + "|" + esc(call->signature.value_or("-")) + "|" +
            esc(normalize_uuids(call->stream_key)) + "|" +
            std::to_string(call->response_output_items);
    }
    if (const auto* calls = std::get_if<sn::ToolCallsRequest>(&event)) {
        std::string out = "toolCalls|" + std::to_string(calls->calls.size()) + "\n";
        for (std::size_t index = 0; index < calls->calls.size(); ++index) {
            if (index > 0) out += "\n";
            out += event_line(sn::StreamEvent{calls->calls[index]});
        }
        return out;
    }
    if (const auto* error = std::get_if<sn::ErrorEvent>(&event)) {
        return "error|" + error_line(error->error);
    }
    if (const auto* usage = std::get_if<sn::UsageUpdate>(&event)) {
        return "usage|" + std::to_string(usage->total_token_count) + "|" +
            (usage->reasoning_token_count.has_value()
                 ? std::to_string(*usage->reasoning_token_count)
                 : std::string("-"));
    }
    if (const auto* retrying = std::get_if<sn::Retrying>(&event)) {
        return "retrying|" + std::to_string(retrying->attempt) + "|" +
            std::to_string(retrying->max_attempts);
    }
    if (const auto* update = std::get_if<sn::ToolCallUpdate>(&event)) {
        return "toolCallUpdate|" + esc(normalize_uuids(update->stream_key)) + "|" +
            esc(normalize_uuids(update->id.value_or("-"))) + "|" + esc(update->name) + "|" +
            esc(update->arguments) + "|" + esc(update->signature.value_or("-"));
    }
    if (const auto* hosted = std::get_if<sn::HostedToolCallUpdate>(&event)) {
        return "hosted|" + esc(normalize_uuids(hosted->stream_key)) + "|" + esc(hosted->name) +
            "|" + esc(hosted->arguments) + "|" + esc(hosted->result.value_or("-")) + "|" +
            (hosted->is_error ? "1" : "0");
    }
    return "citation";
}

// -- scenario runner ----------------------------------------------------------------------------

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
        for (const std::string& field_value : fields) {
            if (!first) out.push_back('|');
            out += field_value;
            first = false;
        }
        out.push_back('\n');
    };
    const auto record = [&out](const sn::StreamEvent& event) {
        out += event_line(event);
        out.push_back('\n');
    };

    line({"name", esc(scenario.name)});
    line({"mode", esc(scenario.mode)});

    DeterministicUuids uuids;
    try {
        if (scenario.mode == "normalizer") {
            sn::ProviderStreamNormalizer normalizer(
                scenario.tools,
                scenario.native_text_parsing_authoritative,
                uuids.function());
            const sn::ProviderStreamNormalizer::Downstream downstream = [&record](const sn::StreamEvent& event) {
                record(event);
            };
            for (const ScenarioEvent& event : scenario.events) {
                normalizer.emit(build_event(event), downstream);
            }
            normalizer.finish(downstream, scenario.release_text_tools);
        } else if (scenario.mode == "thinkingParser") {
            sn::IncrementalThinkingParser parser(scenario.start_in_thinking);
            std::string pending_text;
            std::string pending_thought;
            const sn::IncrementalThinkingParser::TextSink on_text = [&pending_text](const std::string& text) {
                pending_text += text;
            };
            const sn::IncrementalThinkingParser::TextSink on_thought = [&pending_thought](const std::string& thought) {
                pending_thought += thought;
            };
            for (std::size_t index = 0; index < scenario.chunks.size(); ++index) {
                pending_text.clear();
                pending_thought.clear();
                parser.feed(scenario.chunks[index], scenario.thinking_enabled, on_text, on_thought);
                line({"chunk", std::to_string(index)});
                if (!pending_text.empty()) line({"text", esc(pending_text)});
                if (!pending_thought.empty()) line({"thought", esc(pending_thought)});
                line({"pending", esc(parser.pending_content())});
            }
            pending_text.clear();
            pending_thought.clear();
            if (!scenario.flush_thinking_enabled.has_value()) {
                parser.flush(on_text, on_thought);
            } else {
                parser.flush(*scenario.flush_thinking_enabled, on_text, on_thought);
            }
            out += "flush\n";
            if (!pending_text.empty()) line({"text", esc(pending_text)});
            if (!pending_thought.empty()) line({"thought", esc(pending_thought)});
            line({"pending", esc(parser.pending_content())});
            line({"inThinking", parser.in_thinking() ? "1" : "0"});
        } else if (scenario.mode == "textParser") {
            sn::StreamingTextToolCallParser parser(uuids.function());
            sn::StreamingTextToolCallParser::Callbacks callbacks;
            callbacks.on_text = [&line](const std::string& text) { line({"text", esc(text)}); };
            callbacks.on_update = [&line](const sn::StreamingTextToolCallParser::Snapshot& snapshot) {
                line({"update", esc(normalize_uuids(snapshot.stream_key)), esc(snapshot.name),
                      esc(snapshot.arguments)});
            };
            callbacks.on_complete = [&line](const sn::StreamingTextToolCallParser::CompletedCall& call) {
                line({"complete", esc(normalize_uuids(call.stream_key)),
                      esc(normalize_uuids(call.id.value_or("-"))), esc(call.name),
                      esc(call.arguments)});
            };
            callbacks.on_malformed = [&line](const std::string& cause) { line({"malformed", esc(cause)}); };
            for (const std::string& chunk : scenario.chunks) {
                parser.feed(chunk, callbacks);
            }
            parser.flush(callbacks);
        } else if (scenario.mode == "toolParser") {
            const std::vector<sn::ParsedTextCall> calls = sn::parse_tool_call_text(scenario.content);
            for (const sn::ParsedTextCall& call : calls) {
                line({"call", esc(call.id.value_or("-")), esc(call.name), esc(call.arguments)});
            }
            if (calls.empty()) line({"calls", "0"});
        } else if (scenario.mode == "accumulator") {
            sn::ToolArgumentAccumulator accumulator;
            for (const std::optional<std::string>& fragment : scenario.fragments) {
                accumulator.append(fragment.has_value()
                                      ? std::optional<std::string_view>(*fragment)
                                      : std::nullopt);
            }
            line({"value", esc(accumulator.str())});
            line({"empty", accumulator.is_empty() ? "1" : "0"});
        } else if (scenario.mode == "closeBoundary") {
            const std::optional<sn::ImplicitThinkingCloseBoundary> boundary =
                sn::find_implicit_thinking_close(scenario.text);
            if (!boundary.has_value()) {
                line({"boundary", "none"});
            } else {
                line({"boundary", std::to_string(boundary->start_index),
                      std::to_string(boundary->end_exclusive)});
            }
        } else if (scenario.mode == "termination") {
            sn::StreamTermination termination;
            termination.saw_terminal_marker = scenario.saw_terminal_marker;
            termination.stop_reason = scenario.stop_reason;
            termination.produced_content = scenario.produced_content;
            termination.tool_call_in_flight = scenario.tool_call_in_flight;
            if (scenario.has_stream_error) {
                termination.stream_error = sn::GenerationError{sn::ErrorApi{
                    std::nullopt, std::string("relay"), scenario.stream_error_message}};
            }
            termination.already_reported_error = scenario.already_reported_error;
            termination.timed_out = scenario.timed_out;
            termination.retryable_stream_error = scenario.retryable_stream_error;
            line({"truncated", termination.truncated_by_token_cap() ? "1" : "0"});
            line({"retryable", termination.is_retryable() ? "1" : "0"});
            const std::optional<sn::GenerationError> error = termination.to_error("OpenAI");
            line({"error", error.has_value() ? error_line(*error) : "none"});
        } else if (scenario.mode == "malformed") {
            const sn::ToolCallRequest event = sn::malformed_tool_call_request(
                scenario.cause,
                scenario.original_name,
                scenario.original_arguments,
                scenario.stream_key,
                scenario.signature,
                uuids.function());
            line({"event", esc(normalize_uuids(event.id)), esc(event.name), esc(event.arguments),
                  esc(event.signature.value_or("-")), esc(normalize_uuids(event.stream_key))});
            line({"result", esc(sn::malformed_tool_call_result_text(event.arguments))});
        } else if (scenario.mode == "carriesOutput") {
            if (scenario.events.empty()) scenario_fail(stem, "carriesOutput requires one event");
            line({"output", sn::carries_model_output(build_event(scenario.events.front())) ? "1" : "0"});
        } else if (scenario.mode == "retry") {
            line({"delay", std::to_string(sn::ProviderRetryPolicy::delay_millis(scenario.retry_number))});
            line({"upstreamFailure",
                  sn::ProviderRetryPolicy::is_retryable_upstream_failure(scenario.body) ? "1" : "0"});
            const std::set<int> codes(
                scenario.retryable_status_codes.begin(),
                scenario.retryable_status_codes.end());
            line({"retryHttp",
                  sn::ProviderRetryPolicy::should_retry_http(scenario.status_code, scenario.body, codes)
                      ? "1"
                      : "0"});
            const sn::GenerationError stream_error = sn::ErrorApi{
                std::nullopt, std::string("relay"), scenario.stream_error_message};
            line({"streamRetry",
                  sn::ProviderRetryPolicy::should_retry_stream_error(&stream_error) ? "1" : "0"});
        } else {
            scenario_fail(stem, "unknown mode: " + scenario.mode);
        }
    } catch (const sn::IllegalStateError&) {
        // Kotlin error()/check() maps to IllegalStateException in the reference trace.
        line({"error", "IllegalStateException"});
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

TEST(stream_normalization_golden_parity) {
    const std::filesystem::path root = AGORA_FIXTURE_STREAM_NORMALIZATION_DIR;
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
