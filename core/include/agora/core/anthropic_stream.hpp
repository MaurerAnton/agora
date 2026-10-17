#pragma once

// C++ port of AnthropicStreamEventRouter plus the Anthropic stream-event model declarations it
// consumes (AnthropicProvider.kt) and the util pieces the router emits through
// (ToolArgumentAccumulator, ProviderRetryPolicy, the malformed-call stand-in — already pinned by
// the stream-normalization module). The citation conversion is the pinned module 4 projection.
//
// Pinned byte for byte by core/tools/golden/anthropic-stream (the Kotlin reference traces); see
// core/tests/anthropic_stream_parity_test.cpp. The transport half of the provider (socket reads,
// read timeouts, cancellation, HTTP error statuses, the retry loop) is out of scope: this module
// owns everything that decides what a decoded event MEANS and what the stream has proven about
// its own end.
//
// Boundaries (documented divergences, none of them reachable from the frozen corpus):
//  * stop_reason lowercase() is ASCII-only, consistent with the rest of the core port.
//  * Text is UTF-8 at the boundary; lone surrogates cannot be represented. captureParseError's
//    take(512) counts UTF-16 code units and a split pair becomes '?', exactly like the JVM's
//    UTF-8 encoder renders it (json::take_utf16_units).

#include <cstdint>
#include <map>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <variant>
#include <vector>

#include "agora/core/citations.hpp"
#include "agora/core/json.hpp"
#include "agora/core/token_usage.hpp"
#include "agora/core/stream_normalization.hpp"

namespace agora::core::anthropic_stream {

// ---------------------------------------------------------------------------------------------
// The @Serializable stream-event models (AnthropicProvider.kt; extracted verbatim by the stand).

struct Usage {
    std::optional<std::int32_t> input_tokens;
    std::optional<std::int32_t> output_tokens;
    std::optional<std::int32_t> cache_creation_input_tokens;
    std::optional<std::int32_t> cache_read_input_tokens;
};

struct StreamErrorPayload {
    std::optional<std::string> type;
    std::optional<std::string> message;
};

struct Delta {
    std::optional<std::string> text;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
    std::optional<std::string> partial_json;
    std::optional<json::Value> citation;
    std::optional<std::string> type;
    std::optional<std::string> stop_reason;
    std::optional<std::string> stop_sequence;
};

struct ContentBlock {
    std::string type;
    std::optional<std::string> id;
    std::optional<std::string> name;
    std::optional<json::Value> input;
    std::optional<std::string> text;
    std::optional<std::vector<json::Value>> citations;
    std::optional<std::string> thinking;
    std::optional<std::string> signature;
};

struct MessageInfo {
    std::optional<Usage> usage;
    std::optional<std::string> stop_reason;
};

struct AnthropicStreamEvent {
    std::string type;
    std::optional<Delta> delta;
    std::optional<ContentBlock> content_block;
    std::optional<MessageInfo> message;
    std::optional<Usage> usage;
    std::optional<std::int32_t> index;
    std::optional<std::string> outcome;
    std::optional<StreamErrorPayload> error;
};

/** Why a payload did not decode; kotlinx distinguishes a missing field from a type rejection,
 *  which the provider's debug log carries. Both stop the loop with a captured SseParse error. */
struct DecodeFailure {
    bool missing_field = false;
    std::string message;
};

/** decodeFromString<AnthropicStreamEvent> with the provider's Json configuration
 *  (ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false). */
std::optional<AnthropicStreamEvent> decode_stream_event(
    std::string_view source,
    DecodeFailure* failure = nullptr);

// ---------------------------------------------------------------------------------------------
// The StreamEvent kinds the router emits (the provider's StreamEvent subclasses).

/** model/TokenUsage.kt — shared across the stream router modules (see token_usage.hpp). */
using TokenUsage = agora::core::TokenUsage;

struct TextChunk {
    std::string text;
};

struct ThoughtChunk {
    std::string thought;
    std::optional<std::string> title;
    std::optional<std::string> signature;
};

struct CitationUpdate {
    citations::CitationRecord citation;
};

struct UsageUpdate {
    TokenUsage usage;
};

struct ToolCallUpdate {
    std::string stream_key;
    std::optional<std::string> id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
};

struct ToolCallRequest {
    std::string id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
    std::string stream_key;
};

struct ErrorEvent {
    stream_normalization::GenerationError error;
};

using StreamEvent = std::variant<
    TextChunk,
    ThoughtChunk,
    CitationUpdate,
    UsageUpdate,
    ToolCallUpdate,
    ToolCallRequest,
    ErrorEvent>;

/** util.StreamEvent.carriesModelOutput. */
bool carries_model_output(const StreamEvent& event);

/** kotlinx JsonElement.equals: primitives by isString+content, arrays element-wise, objects as
 *  maps with duplicate keys collapsed last-wins (the shape the lexer builds). */
bool json_element_equal(const json::Value& first, const json::Value& second);

// ---------------------------------------------------------------------------------------------
// AnthropicStreamEventRouter.

class AnthropicStreamEventRouter {
public:
    using UuidFn = stream_normalization::UuidFn;

    explicit AnthropicStreamEventRouter(UuidFn uuid = {});

    /** route(event): the events this one protocol event produces, in emission order. */
    std::vector<StreamEvent> route(const AnthropicStreamEvent& event);

    /** Surface malformed open blocks without turning any unclosed block into an executable
     *  call. Every call finds the same open block again (the Kotlin method is not one-shot). */
    std::vector<StreamEvent> report_incomplete_blocks();

    /** captureParseError: the first failure wins; raw_line is cut to 512 UTF-16 units. */
    void capture_parse_error(const std::string& raw_line, const std::string& cause);

    /** A `message_stop` event was received. */
    bool message_stop_received = false;

    /** Normalized `stop_reason`, read from `message_delta.delta` per protocol. */
    std::optional<std::string> stop_reason;

    int tool_use_block_starts = 0;

    /** Failure the provider reported inside the 200 stream. */
    std::optional<stream_normalization::GenerationError> stream_error;

    /** True once the router has already emitted a fatal Error event itself. */
    bool reported_error = false;

    bool saw_terminal_marker() const;

    bool tool_call_in_flight() const;

private:
    struct ToolBlock {
        std::string stream_key;
        std::optional<std::string> id;
        std::string name;
        stream_normalization::ToolArgumentAccumulator arguments;
    };

    struct TextBlock {
        std::int32_t start_index = 0;
        std::string text;
        std::vector<json::Value> citations;
    };

    void route_untyped_delta(std::int32_t index, const Delta& delta, std::vector<StreamEvent>& output);
    void append_text(std::int32_t index, const std::string& text, std::vector<StreamEvent>& output);
    void append_citation(std::int32_t index, const json::Value& citation, std::vector<StreamEvent>& output);
    void emit_citation(
        const TextBlock& block,
        const json::Value& citation,
        std::vector<StreamEvent>& output);
    std::int32_t index_for_start(const std::optional<std::int32_t>& index);
    std::int32_t index_for_continuation(const std::optional<std::int32_t>& index);
    std::optional<std::string> invalid_completion_cause(const ToolBlock& tool) const;
    ToolCallUpdate update_event(const ToolBlock& tool) const;
    ToolCallRequest complete_event(const ToolBlock& tool) const;
    std::string next_uuid() const;

    UuidFn uuid_;
    std::map<std::int32_t, std::string> block_types_;
    std::map<std::int32_t, TextBlock> text_blocks_;
    std::map<std::int32_t, ToolBlock> tool_blocks_;
    std::map<std::int32_t, std::optional<std::string>> thinking_signatures_;
    std::set<std::string> completed_tool_call_ids_;
    std::int32_t last_block_index_ = -1;
    std::int32_t synthetic_index_ = 0;
    std::int32_t answer_length_ = 0;
    std::optional<std::int32_t> input_tokens_;
    std::int32_t cache_creation_input_tokens_ = 0;
    std::int32_t cache_read_input_tokens_ = 0;
};

}  // namespace agora::core::anthropic_stream
