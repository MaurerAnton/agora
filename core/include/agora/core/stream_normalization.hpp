#pragma once

// C++ port of Agora's stream post-processing pipeline: the provider-event normalizer, the
// incremental thinking lexer, the text tool-call parsers, stream termination decisions, the
// defensive tool-argument accumulator and the malformed-call stand-in.
//
// Pinned byte for byte by core/tools/golden/stream-normalization (the Kotlin reference traces);
// see core/tests/stream_normalization_parity_test.cpp. Kotlin's suspend callbacks become plain
// synchronous sinks because the whole pipeline is sequential.

#include <cstddef>
#include <cstdint>
#include <functional>
#include <map>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

namespace agora::core::stream_normalization {

// ---------------------------------------------------------------------------------------------
// GenerationError (the kinds this pipeline can produce or pass through).

struct ErrorNetwork {
    int status_code = 0;
    std::string message;
};

struct ErrorApi {
    std::optional<std::string> code;
    std::optional<std::string> type;
    std::string message;
};

struct ErrorSseParse {
    std::string raw_line;
    std::string cause;
};

struct ErrorMalformedToolCall {
    std::string cause;
};

struct ErrorIncompleteStream {
    std::string provider;
    std::optional<std::string> stop_reason;
    bool tool_call_in_flight = false;
    bool produced_content = false;
};

struct ErrorOutputTruncated {
    std::string provider;
    std::optional<std::string> stop_reason;
};

struct ErrorTimeout {};

using GenerationError = std::variant<
    ErrorNetwork,
    ErrorApi,
    ErrorSseParse,
    ErrorMalformedToolCall,
    ErrorIncompleteStream,
    ErrorOutputTruncated,
    ErrorTimeout>;

// ---------------------------------------------------------------------------------------------
// StreamEvent.

struct TextChunk {
    std::string text;
};

struct ThoughtChunk {
    std::string thought;
    std::optional<std::string> title;
    std::optional<std::string> signature;
};

struct ToolCallRequest {
    std::string id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
    std::string stream_key;
    std::size_t response_output_items = 0;
};

struct ToolCallsRequest {
    std::vector<ToolCallRequest> calls;
};

struct ErrorEvent {
    GenerationError error;
};

struct UsageUpdate {
    int total_token_count = 0;
    std::optional<int> reasoning_token_count;
};

struct Retrying {
    int attempt = 0;
    int max_attempts = 0;
};

struct ToolCallUpdate {
    std::string stream_key;
    std::optional<std::string> id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
};

struct HostedToolCallUpdate {
    std::string stream_key;
    std::string name;
    std::string arguments;
    std::optional<std::string> result;
    bool is_error = false;
};

/** Citation payloads pass through the normalizer; their fields are not consumed by this module. */
struct CitationUpdate {};

using StreamEvent = std::variant<
    TextChunk,
    ThoughtChunk,
    ToolCallRequest,
    ToolCallsRequest,
    ErrorEvent,
    UsageUpdate,
    Retrying,
    ToolCallUpdate,
    HostedToolCallUpdate,
    CitationUpdate>;

// ---------------------------------------------------------------------------------------------
// Shared plumbing.

/** Kotlin `error(...)` / `check(...)`: the trace driver maps this to IllegalStateException. */
struct IllegalStateError : std::logic_error {
    explicit IllegalStateError(const std::string& what) : std::logic_error(what) {}
};

/** Returns a dashed lowercase UUID v4; injectable for deterministic tests (as openai_stream). */
using UuidFn = std::function<std::string()>;

/** Production default: a random dashed uuid (UUID.randomUUID().toString()). */
std::string random_uuid_v4();

/** UUID.randomUUID().toString().replace("-", ""). */
std::string compact_uuid(std::string_view dashed);

// ---------------------------------------------------------------------------------------------
// IncrementalThinkingParser: incremental lexer for model-emitted reasoning delimiters.

class IncrementalThinkingParser {
public:
    using TextSink = std::function<void(const std::string&)>;

    /** Marker definition; public so the sibling translation units share the matcher helpers. */
    struct Marker;

    explicit IncrementalThinkingParser(bool start_in_thinking = false);

    bool in_thinking() const;
    const std::string& pending_content() const;

    void feed(
        const std::string& content,
        bool thinking_enabled,
        const TextSink& on_text,
        const TextSink& on_thought);

    /** Kotlin default argument: flush with the last feed's thinking_enabled. */
    void flush(const TextSink& on_text, const TextSink& on_thought);
    void flush(bool thinking_enabled, const TextSink& on_text, const TextSink& on_thought);

    void reset();

private:
    void drain(bool final, bool thinking_enabled, const TextSink& on_text, const TextSink& on_thought);
    void seed_implicit_marker();

    std::vector<const Marker*> marker_stack_;
    bool implicit_boundary_closed_ = false;
    int code_state_kind_ = 0;  // 0 none, 1 inline, 2 fence
    char code_fence_character_ = 0;
    int code_ticks_ = 0;
    int code_fence_length_ = 0;
    std::string pending_;
    bool line_prefix_ = true;
    int line_indent_ = 0;
    bool indented_code_line_ = false;
    bool last_thinking_enabled_ = true;
    bool start_in_thinking_ = false;
};

/** Unified local/Ollama facade over IncrementalThinkingParser (delegation only). */
class ThinkingParser {
public:
    using TextSink = IncrementalThinkingParser::TextSink;

    void feed(
        const std::string& content,
        bool thinking_enabled,
        const TextSink& on_text,
        const TextSink& on_thought);
    void flush(const TextSink& on_text, const TextSink& on_thought);
    void reset();

private:
    IncrementalThinkingParser parser_;
};

/** OpenAI-compatible facade over the same lexer; flush's nullable switch mirrors the Kotlin one. */
class StreamingThinkTagParser {
public:
    using TextSink = IncrementalThinkingParser::TextSink;

    bool in_thinking_block() const;
    std::string pending_buffer() const;

    void feed(
        const std::string& content,
        bool thinking_enabled,
        const TextSink& on_text,
        const TextSink& on_thought);
    void flush(const TextSink& on_text, const TextSink& on_thought, std::optional<bool> thinking_enabled = std::nullopt);

private:
    IncrementalThinkingParser parser_;
};

// ---------------------------------------------------------------------------------------------
// StreamingTextToolCallParser.

class StreamingTextToolCallParser {
public:
    struct Snapshot {
        std::string stream_key;
        std::string name;
        std::string arguments;

        friend bool operator==(const Snapshot& lhs, const Snapshot& rhs) {
            return lhs.stream_key == rhs.stream_key && lhs.name == rhs.name &&
                lhs.arguments == rhs.arguments;
        }
    };

    struct CompletedCall {
        std::string stream_key;
        std::optional<std::string> id;
        std::string name;
        std::string arguments;
    };

    struct Callbacks {
        std::function<void(const std::string&)> on_text;
        std::function<void(const Snapshot&)> on_update;
        std::function<void(const CompletedCall&)> on_complete;
        std::function<void(const std::string&)> on_malformed;
    };

    explicit StreamingTextToolCallParser(UuidFn uuid = {});

    void feed(const std::string& content, const Callbacks& callbacks);
    void flush(const Callbacks& callbacks);

private:
    enum class Mode { kNormal, kTaggedCall, kBareJsonCall };
    enum class TaggedProtocol { kJson, kXml };

    void drain(const Callbacks& callbacks);
    void complete_bare_json(const Callbacks& callbacks);
    void announce_partial(const std::string& body, const Callbacks& callbacks);
    void emit_snapshot(const std::string& key, const std::string& name, const std::string& arguments, const Callbacks& callbacks);
    void begin_call(Mode next_mode, const Callbacks& callbacks);
    void reset_after_call();
    std::string new_stream_key() const;
    std::optional<std::pair<std::size_t, TaggedProtocol>> find_tagged_start() const;
    std::optional<std::string> xml_close_tag() const;
    std::size_t longest_tag_prefix_suffix() const;

    UuidFn uuid_;
    std::string buffer_;
    Mode mode_ = Mode::kNormal;
    bool can_probe_bare_json_ = true;
    std::optional<std::string> stream_key_;
    std::optional<std::string> announced_name_;
    std::string announced_arguments_;
    std::optional<Snapshot> last_snapshot_;
    TaggedProtocol active_tagged_protocol_ = TaggedProtocol::kJson;
};

// ---------------------------------------------------------------------------------------------
// ToolCallTextParser.parse.

struct ParsedTextCall {
    std::optional<std::string> id;
    std::string name;
    std::string arguments;
};

/** Extract tool calls from content text; empty if none are recognized (all-or-nothing). */
std::vector<ParsedTextCall> parse_tool_call_text(std::string_view content);

// ---------------------------------------------------------------------------------------------
// StreamTermination.

struct StreamTermination {
    bool saw_terminal_marker = false;
    std::optional<std::string> stop_reason;
    bool produced_content = false;
    bool tool_call_in_flight = false;
    std::optional<GenerationError> stream_error;
    bool already_reported_error = false;
    bool timed_out = false;
    bool retryable_stream_error = true;

    bool truncated_by_token_cap() const;
    bool is_retryable() const;
    std::optional<GenerationError> to_error(const std::string& provider) const;
    std::string describe() const;
};

bool carries_model_output(const StreamEvent& event);

// ---------------------------------------------------------------------------------------------
// ToolArgumentAccumulator.

class ToolArgumentAccumulator {
public:
    explicit ToolArgumentAccumulator(std::string initial = {});

    bool is_empty() const;
    void append(std::optional<std::string_view> fragment);
    void replace(std::string value);
    const std::string& str() const;

private:
    std::string builder_;
};

// ---------------------------------------------------------------------------------------------
// findImplicitThinkingClose.

struct ImplicitThinkingCloseBoundary {
    std::size_t start_index = 0;
    std::size_t end_exclusive = 0;
};

std::optional<ImplicitThinkingCloseBoundary> find_implicit_thinking_close(std::string_view content);

// ---------------------------------------------------------------------------------------------
// MalformedToolCall.

extern const std::string_view kMalformedToolCallName;  // agora_malformed_tool_call

ToolCallRequest malformed_tool_call_request(
    const std::string& cause,
    std::optional<std::string> original_name = {},
    std::optional<std::string> original_arguments = {},
    std::optional<std::string> stream_key = {},
    std::optional<std::string> signature = {},
    UuidFn uuid = {});

std::string malformed_tool_call_result_text(std::string_view arguments);

// ---------------------------------------------------------------------------------------------
// ProviderRetryPolicy.

struct ProviderRetryPolicy {
    static constexpr int kMaxRetries = 5;
    static constexpr int kMaxAttempts = kMaxRetries + 1;

    /** Delay before retry number; throws IllegalStateError outside 1..5. */
    static std::int64_t delay_millis(int retry_number);

    static bool is_failed_to_generate_outcome(const std::optional<std::string>& raw);

    /** Exact response-body read failure eligible for pre-output Provider replay. */
    static bool is_unreadable_server_response_failure(const std::optional<std::string>& raw);

    static bool is_retryable_upstream_failure(const std::optional<std::string>& raw);

    static bool should_retry_http(
        int status_code,
        const std::optional<std::string>& body,
        const std::set<int>& retryable_status_codes);

    static bool should_retry_stream_error(const GenerationError* error);
};

// ---------------------------------------------------------------------------------------------
// ProviderStreamNormalizer.

class ProviderStreamNormalizer {
public:
    using Downstream = std::function<void(const StreamEvent&)>;

    explicit ProviderStreamNormalizer(
        std::vector<std::string> offered_tool_names,
        bool native_text_parsing_authoritative = false,
        UuidFn uuid = {});

    void emit(const StreamEvent& event, const Downstream& downstream);
    void finish(const Downstream& downstream, bool release_text_tools = true);

private:
    struct TextToolCandidate {
        std::string stream_key;
        std::optional<std::string> id;
        std::string name;
        std::string arguments;
    };

    struct ToolFingerprint {
        std::string name;
        std::string arguments;

        friend bool operator==(const ToolFingerprint& lhs, const ToolFingerprint& rhs) {
            return lhs.name == rhs.name && lhs.arguments == rhs.arguments;
        }
    };

    enum class ToolProbe { kUndecided, kTagged, kJson, kText };

    void route_provider_text(const std::string& content, const Downstream& downstream);
    void route_raw_text(const std::string& content, const Downstream& downstream);
    void route_structured_thought(const ThoughtChunk& event, const Downstream& downstream);
    void flush_thinking(const Downstream& downstream);
    void flush_structured_thinking(const Downstream& downstream);
    void flush_inline_thinking(const Downstream& downstream);
    void emit_inline_thought(const std::string& content, const Downstream& downstream);
    void emit_native_thought(
        const std::string& content,
        const std::optional<std::string>& title,
        const std::optional<std::string>& signature,
        const Downstream& downstream);
    void route_tool_text(const std::string& content, const Downstream& downstream);
    void decide_tool_probe(const Downstream& downstream);
    void feed_tagged(const std::string& content);
    void add_text_candidate(const StreamingTextToolCallParser::CompletedCall& call);
    void finish_text_tools(const Downstream& downstream);
    void finish_json_tool(const Downstream& downstream);
    void release_text_tools(const Downstream& downstream);
    void record_native_call(const ToolCallRequest& call);
    std::optional<ToolFingerprint> fingerprint(const std::string& name, const std::string& arguments) const;
    void report_malformed(const std::string& cause, const std::optional<std::string>& raw = {});
    void flush_aborted_text(const Downstream& downstream);
    static bool looks_like_explicit_json_tool(const std::string& content);

    const std::set<std::string> offered_tool_names_;
    IncrementalThinkingParser inline_thinking_;
    IncrementalThinkingParser structured_thinking_;
    std::optional<StreamingTextToolCallParser> text_tool_parser_;
    std::string tool_probe_buffer_;
    std::string tagged_residual_text_;
    std::vector<TextToolCandidate> text_tool_candidates_;
    std::set<std::pair<std::string, std::string>> native_fingerprints_;
    std::set<std::string> native_call_ids_;
    std::map<std::string, std::optional<ToolFingerprint>> native_fingerprints_by_id_;
    std::string inline_thought_;
    std::string native_thought_;
    std::string inline_visible_projection_;
    std::string native_visible_projection_;
    std::optional<std::string> structured_title_;
    std::optional<std::string> structured_signature_;
    ToolProbe tool_probe_ = ToolProbe::kUndecided;
    bool malformed_text_tool_ = false;
    std::vector<ToolCallRequest> malformed_text_calls_;
    bool finished_ = false;
    UuidFn uuid_;
};

}  // namespace agora::core::stream_normalization
