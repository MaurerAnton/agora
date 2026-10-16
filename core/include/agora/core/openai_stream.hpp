#pragma once

#include <cstdint>
#include <functional>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::openai {

/**
 * One way to consume one HTTP 200 chat-completions SSE stream and report how it
 * went, ported line for line from the pinned Kotlin reference
 * (OpenAiChatStreamConsumer.kt, core/tools/golden/openai).
 *
 * The provider owns the socket, the clock and the cancellation; this class owns
 * everything that decides what the bytes MEAN: delta parsing, tool-call
 * accumulation, terminal markers and the semantic verdict. The golden traces
 * under core/tools/golden/openai are the parity contract: the same transcript
 * must produce the same event stream and the same termination byte for byte.
 */

/** Token usage mirroring model.TokenUsage: counts are Kotlin Ints. */
struct TokenUsage {
    int32_t total_token_count = 0;
    std::optional<int32_t> input_token_count;
    std::optional<int32_t> cached_input_token_count;
    std::optional<int32_t> cache_write_input_token_count;
    std::optional<int32_t> uncached_input_token_count;
    std::optional<int32_t> output_token_count;
    std::optional<int32_t> reasoning_token_count;
};

/** A complete tool call ready for local execution. */
struct ToolCall {
    std::string id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
    std::string stream_key;
};

/** The GenerationError subset this consumer can produce or carry. */
struct Error {
    enum class Kind {
        None,
        Network,
        Api,
        SseParse,
        MalformedToolCall,
        IncompleteStream,
        OutputTruncated,
        Timeout,
    };

    Kind kind = Kind::None;
    int32_t status_code = 0;                 // Network
    std::string message;                     // Network / Api
    std::optional<std::string> code;         // Api
    std::optional<std::string> type;         // Api
    std::string raw_line;                    // SseParse
    std::string cause;                       // SseParse / MalformedToolCall
    std::string provider;                    // IncompleteStream / OutputTruncated
    std::optional<std::string> stop_reason;  // IncompleteStream / OutputTruncated
    bool tool_call_in_flight = false;        // IncompleteStream
    bool produced_content = false;           // IncompleteStream
};

/** One stream event; mirrors the Kotlin StreamEvent subclasses the consumer emits. */
struct Event {
    enum class Kind {
        TextChunk,
        ThoughtChunk,
        ToolCallUpdate,
        ToolCallRequest,
        ToolCallsRequest,
        UsageUpdate,
        Error,
    };

    Kind kind = Kind::TextChunk;
    std::string text;                      // TextChunk.text / ThoughtChunk.thought
    std::optional<std::string> title;      // ThoughtChunk
    std::optional<std::string> signature;  // ThoughtChunk
    std::string stream_key;                // ToolCallUpdate
    std::optional<std::string> call_id;    // ToolCallUpdate
    std::string name;                      // ToolCallUpdate / ToolCallRequest
    std::string arguments;                 // ToolCallUpdate / ToolCallRequest
    std::vector<ToolCall> calls;           // ToolCallsRequest
    TokenUsage usage;                      // UsageUpdate
    Error error;                           // Error
};

/** How the stream actually ended; mirrors util.StreamTermination. */
struct Termination {
    bool saw_terminal_marker = false;
    std::optional<std::string> stop_reason;
    bool produced_content = false;
    bool tool_call_in_flight = false;
    bool already_reported_error = false;
    bool timed_out = false;
    bool retryable_stream_error = true;
    Error stream_error;

    bool truncated_by_token_cap() const;
    bool is_retryable() const;
    Error to_error(std::string_view provider) const;
    std::string describe() const;
};

/**
 * Accumulates the streamed arguments of one tool call defensively, mirroring
 * util.ToolArgumentAccumulator: empty fragments are ignored, a growing
 * snapshot replaces the shorter value, a long repeated snapshot is ignored.
 */
struct ToolArgumentAccumulator {
    static constexpr std::size_t kStaleSnapshotMinLength = 16;

    std::string value;

    bool empty() const { return value.empty(); }
    std::string to_string() const { return value; }

    void append(std::string_view fragment);
};

class ChatStreamConsumer {
public:
    struct Options {
        std::string provider_name = "agora";
        /** Returns a canonical dashed UUID string; injectable for determinism. */
        std::function<std::string()> uuid;
        /** Diagnostics sink for malformed payloads; may be empty. */
        std::function<void(const std::string&)> log_malformed;
    };

    ChatStreamConsumer();
    explicit ChatStreamConsumer(Options options);

    /** One line of the response body, verbatim (without the trailing newline). */
    struct LineOutcome {
        bool keep_reading = true;
        std::vector<Event> events;
    };

    LineOutcome consume_line(std::string_view line);

    /** A read deadline expired; returns true when reading should continue. */
    bool on_read_timeout(bool terminal_grace_active);

    /** The response body failed to read; recorded and the loop must stop. */
    void on_read_error(Error error);

    /** A semantic end-of-message marker (finish_reason / [DONE]) has arrived. */
    bool has_semantic_terminal() const { return finish_reason_.has_value(); }

    Termination finish() const;

private:
    struct PendingToolCall {
        std::string stream_key;
        std::string id;
        std::string name;
        ToolArgumentAccumulator args;
    };

    void emit_tracked(LineOutcome& outcome, Event event);
    void emit_pending(LineOutcome& outcome);
    PendingToolCall& get_or_create(int32_t index);
    std::string next_uuid() const;

    Options options_;
    bool saw_done_ = false;
    std::optional<std::string> finish_reason_;
    bool produced_content_ = false;
    bool reported_error_ = false;
    bool timed_out_ = false;
    Error stream_error_;
    int consecutive_read_timeouts_ = 0;
    std::vector<std::pair<int32_t, PendingToolCall>> pending_;
};

}  // namespace agora::core::openai
