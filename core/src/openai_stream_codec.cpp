#include "agora/core/openai_stream_codec.hpp"

namespace agora::core::openai {
namespace {

constexpr std::uint32_t kNullString = 0xFFFFFFFFu;

void append_u8(std::vector<std::uint8_t>& out, std::uint8_t value) {
    out.push_back(value);
}

void append_u16(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_u32(std::vector<std::uint8_t>& out, std::uint32_t value) {
    out.push_back(static_cast<std::uint8_t>((value >> 24) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 16) & 0xFF));
    out.push_back(static_cast<std::uint8_t>((value >> 8) & 0xFF));
    out.push_back(static_cast<std::uint8_t>(value & 0xFF));
}

void append_i32(std::vector<std::uint8_t>& out, std::int32_t value) {
    append_u32(out, static_cast<std::uint32_t>(value));
}

void append_string(std::vector<std::uint8_t>& out, const std::string& value) {
    append_u32(out, static_cast<std::uint32_t>(value.size()));
    out.insert(out.end(), value.begin(), value.end());
}

void append_optional_string(std::vector<std::uint8_t>& out,
                            const std::optional<std::string>& value) {
    if (!value) {
        append_u32(out, kNullString);
        return;
    }
    append_string(out, *value);
}

void append_optional_int(std::vector<std::uint8_t>& out,
                         const std::optional<std::int32_t>& value) {
    if (!value) {
        append_u8(out, 0);
        return;
    }
    append_u8(out, 1);
    append_i32(out, *value);
}

void append_tool_call(std::vector<std::uint8_t>& out, const ToolCall& call) {
    append_string(out, call.id);
    append_string(out, call.name);
    append_string(out, call.arguments);
    append_optional_string(out, call.signature);
    append_string(out, call.stream_key);
}

void append_usage(std::vector<std::uint8_t>& out, const TokenUsage& usage) {
    append_i32(out, usage.total_token_count);
    append_optional_int(out, usage.input_token_count);
    append_optional_int(out, usage.cached_input_token_count);
    append_optional_int(out, usage.cache_write_input_token_count);
    append_optional_int(out, usage.uncached_input_token_count);
    append_optional_int(out, usage.output_token_count);
    append_optional_int(out, usage.reasoning_token_count);
}

void append_error(std::vector<std::uint8_t>& out, const Error& error) {
    switch (error.kind) {
        case Error::Kind::None:
            append_u8(out, 0);
            return;
        case Error::Kind::Network:
            append_u8(out, 1);
            append_i32(out, error.status_code);
            append_string(out, error.message);
            return;
        case Error::Kind::Api:
            append_u8(out, 2);
            append_optional_string(out, error.code);
            append_optional_string(out, error.type);
            append_string(out, error.message);
            return;
        case Error::Kind::SseParse:
            append_u8(out, 3);
            append_string(out, error.raw_line);
            append_string(out, error.cause);
            return;
        case Error::Kind::MalformedToolCall:
            append_u8(out, 4);
            append_string(out, error.cause);
            return;
        case Error::Kind::IncompleteStream:
            append_u8(out, 5);
            append_string(out, error.provider);
            append_optional_string(out, error.stop_reason);
            append_u8(out, error.tool_call_in_flight ? 1 : 0);
            append_u8(out, error.produced_content ? 1 : 0);
            return;
        case Error::Kind::OutputTruncated:
            append_u8(out, 6);
            append_string(out, error.provider);
            append_optional_string(out, error.stop_reason);
            return;
        case Error::Kind::Timeout:
            append_u8(out, 7);
            return;
    }
}

void append_event(std::vector<std::uint8_t>& out, const Event& event) {
    switch (event.kind) {
        case Event::Kind::TextChunk:
            append_u8(out, 1);
            append_string(out, event.text);
            return;
        case Event::Kind::ThoughtChunk:
            append_u8(out, 2);
            append_string(out, event.text);
            append_optional_string(out, event.title);
            append_optional_string(out, event.signature);
            return;
        case Event::Kind::ToolCallUpdate:
            append_u8(out, 3);
            append_string(out, event.stream_key);
            append_optional_string(out, event.call_id);
            append_string(out, event.name);
            append_string(out, event.arguments);
            return;
        case Event::Kind::ToolCallRequest: {
            append_u8(out, 4);
            ToolCall call;
            call.id = event.call_id.value_or("");
            call.name = event.name;
            call.arguments = event.arguments;
            call.signature = event.signature;
            call.stream_key = event.stream_key;
            append_tool_call(out, call);
            return;
        }
        case Event::Kind::ToolCallsRequest:
            append_u8(out, 5);
            append_u16(out, static_cast<std::uint32_t>(event.calls.size()));
            for (const ToolCall& call : event.calls) append_tool_call(out, call);
            return;
        case Event::Kind::UsageUpdate:
            append_u8(out, 6);
            append_usage(out, event.usage);
            return;
        case Event::Kind::Error:
            append_u8(out, 7);
            append_error(out, event.error);
            return;
    }
}

}  // namespace

std::vector<std::uint8_t> encode_line_outcome(const ChatStreamConsumer::LineOutcome& outcome,
                                              const std::vector<std::string>& logs) {
    std::vector<std::uint8_t> out;
    append_u8(out, outcome.keep_reading ? 1 : 0);
    append_u16(out, static_cast<std::uint32_t>(logs.size()));
    for (const std::string& log : logs) append_string(out, log);
    append_u16(out, static_cast<std::uint32_t>(outcome.events.size()));
    for (const Event& event : outcome.events) append_event(out, event);
    return out;
}

std::vector<std::uint8_t> encode_termination(const Termination& termination) {
    std::vector<std::uint8_t> out;
    append_u8(out, termination.saw_terminal_marker ? 1 : 0);
    append_optional_string(out, termination.stop_reason);
    append_u8(out, termination.produced_content ? 1 : 0);
    append_u8(out, termination.tool_call_in_flight ? 1 : 0);
    append_u8(out, termination.already_reported_error ? 1 : 0);
    append_u8(out, termination.timed_out ? 1 : 0);
    append_error(out, termination.stream_error);
    return out;
}

}  // namespace agora::core::openai
