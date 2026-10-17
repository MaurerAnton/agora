// Unit tests for the C++ stream post-processing port: behavior the golden stand cannot pin via
// stdin scenarios (injected UUIDs, the IllegalStateException mapping, facade delegation,
// idempotent finish, predicate tables). Trace-level behavior is pinned by
// stream_normalization_parity_test.cpp against the frozen Kotlin traces.

#include <cstdint>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/stream_normalization.hpp"
#include "check.hpp"

namespace {

namespace sn = agora::core::stream_normalization;

constexpr std::string_view kOpenToolCall = "<" "tool_" "call>";
constexpr std::string_view kCloseToolCall = "<" "/tool_" "call>";
constexpr std::string_view kCloseThinking = "<" "/thinking>";

/** [A-Za-z0-9_-]{1,128}, matching safeWireToolCallId. */
bool is_safe_call_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

sn::UuidFn counting_uuids() {
    return [] {
        static std::uint64_t counter = 0;
        static constexpr char kHex[] = "0123456789abcdef";
        std::string out = "11111111-2222-4333-8444-";
        const std::uint64_t value = counter++;
        for (int shift = 44; shift >= 0; shift -= 4) {
            out.push_back(kHex[(value >> shift) & 0xF]);
        }
        return out;
    };
}

sn::UuidFn fixed_uuid(std::string value) {
    return [value = std::move(value)]() { return value; };
}

}  // namespace

TEST(stream_normalization_facades_delegate) {
    // ThinkingParser must produce exactly the same callbacks as the lexer it wraps.
    sn::IncrementalThinkingParser direct;
    sn::ThinkingParser facade;
    const std::string chunks[] = {"<thinking" ">", "alpha", std::string(kCloseThinking), "beta"};
    std::string direct_text;
    std::string direct_thought;
    std::string facade_text;
    std::string facade_thought;
    for (const std::string& chunk : chunks) {
        direct.feed(
            chunk,
            true,
            [&](const std::string& text) { direct_text += text; },
            [&](const std::string& thought) { direct_thought += thought; });
        facade.feed(
            chunk,
            true,
            [&](const std::string& text) { facade_text += text; },
            [&](const std::string& thought) { facade_thought += thought; });
    }
    direct.flush(
        [&](const std::string& text) { direct_text += text; },
        [&](const std::string& thought) { direct_thought += thought; });
    facade.flush(
        [&](const std::string& text) { facade_text += text; },
        [&](const std::string& thought) { facade_thought += thought; });
    CHECK_EQ(facade_text, direct_text);
    CHECK_EQ(facade_thought, direct_thought);
    CHECK_EQ(direct_text, std::string("beta"));
    CHECK_EQ(direct_thought, std::string("alpha"));
    facade.reset();
    direct.reset();
    CHECK_EQ(direct.pending_content(), std::string());
}

TEST(stream_normalization_think_tag_facade) {
    sn::StreamingThinkTagParser parser;
    CHECK(!parser.in_thinking_block());
    CHECK_EQ(parser.pending_buffer(), std::string());

    std::string text;
    std::string thought;
    parser.feed(
        "<thinking" ">abc",
        true,
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { thought += value; });
    CHECK(parser.in_thinking_block());
    CHECK_EQ(thought, std::string("abc"));

    // The explicit-flag flush mirrors the nullable overload: disabled thinking swallows thought.
    thought.clear();
    parser.feed(
        std::string("def") + std::string(kCloseThinking),
        true,
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { thought += value; });
    CHECK(!parser.in_thinking_block());
    CHECK_EQ(thought, std::string("def"));

    std::string flush_thought;
    parser.feed(
        "<thinking" ">late",
        true,
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { flush_thought += value; });
    parser.flush(
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { flush_thought += value; });
    CHECK_EQ(flush_thought, std::string("late"));

    parser.feed(
        "<thinking" ">hidden",
        true,
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { flush_thought += value; });
    const std::size_t before = flush_thought.size();
    parser.flush(
        [&](const std::string& value) { text += value; },
        [&](const std::string& value) { flush_thought += value; },
        false);
    CHECK_EQ(flush_thought.size(), before);
}

TEST(stream_normalization_retry_policy) {
    using Policy = sn::ProviderRetryPolicy;
    CHECK_EQ(Policy::delay_millis(1), static_cast<std::int64_t>(5000));
    CHECK_EQ(Policy::delay_millis(3), static_cast<std::int64_t>(5000));
    CHECK_EQ(Policy::delay_millis(4), static_cast<std::int64_t>(30000));
    CHECK_EQ(Policy::delay_millis(5), static_cast<std::int64_t>(30000));
    CHECK_EQ(Policy::kMaxRetries, 5);
    CHECK_EQ(Policy::kMaxAttempts, 6);

    bool thrown = false;
    try {
        Policy::delay_millis(0);
    } catch (const sn::IllegalStateError& error) {
        thrown = true;
        CHECK_EQ(std::string(error.what()), std::string("Invalid retry number: 0"));
    }
    CHECK(thrown);
    thrown = false;
    try {
        Policy::delay_millis(6);
    } catch (const sn::IllegalStateError&) {
        thrown = true;
    }
    CHECK(thrown);

    CHECK(Policy::is_failed_to_generate_outcome("Failed To Generate something"));
    CHECK(!Policy::is_failed_to_generate_outcome(std::nullopt));
    CHECK(Policy::is_unreadable_server_response_failure("THE SERVER RESPONSE COULD NOT BE READ"));
    CHECK(Policy::is_retryable_upstream_failure("upstream: failed to generate"));
    CHECK(Policy::is_retryable_upstream_failure("x the server response could not be read y"));
    CHECK(!Policy::is_retryable_upstream_failure("rate limited"));

    CHECK(Policy::should_retry_http(429, std::nullopt, {429, 500}));
    CHECK(!Policy::should_retry_http(429, std::nullopt, {500}));
    CHECK(Policy::should_retry_http(404, "failed to generate", {}));
    CHECK(!Policy::should_retry_http(404, "nope", {429}));

    CHECK(!Policy::should_retry_stream_error(nullptr));
    const sn::GenerationError api_retryable{
        sn::ErrorApi{std::nullopt, std::string("relay"), "failed to generate"}};
    CHECK(Policy::should_retry_stream_error(&api_retryable));
    const sn::GenerationError api_other{
        sn::ErrorApi{std::nullopt, std::string("relay"), "invalid api key"}};
    CHECK(!Policy::should_retry_stream_error(&api_other));
    const sn::GenerationError network_retryable{
        sn::ErrorNetwork{0, "the server response could not be read"}};
    CHECK(Policy::should_retry_stream_error(&network_retryable));
    const sn::GenerationError sse_parse{sn::ErrorSseParse{"line", "cause"}};
    CHECK(!Policy::should_retry_stream_error(&sse_parse));
}

TEST(stream_normalization_termination) {
    sn::StreamTermination termination;
    CHECK(!termination.truncated_by_token_cap());
    CHECK_EQ(termination.describe(),
             std::string("stop_reason=null terminal_marker=false produced_content=false "
                         "tool_in_flight=false timed_out=false stream_error=false "
                         "reported_error=false retryable=true"));

    termination.stop_reason = "max_tokens";
    CHECK(termination.truncated_by_token_cap());
    termination.stop_reason = "length";
    CHECK(termination.truncated_by_token_cap());
    termination.stop_reason = "max_output_tokens";
    CHECK(termination.truncated_by_token_cap());
    termination.stop_reason = "MAX_TOKENS";
    CHECK(!termination.truncated_by_token_cap());

    // A cap stop is never retried, even when the transport timed out; the timeout itself still
    // takes precedence in the error mapping (Kotlin checks timedOut before the cap).
    termination.stop_reason = "length";
    termination.timed_out = true;
    CHECK(!termination.is_retryable());
    CHECK(std::holds_alternative<sn::ErrorTimeout>(*termination.to_error("OpenAI")));
    termination.timed_out = false;
    const std::optional<sn::GenerationError> truncated = termination.to_error("OpenAI");
    CHECK(truncated.has_value());
    CHECK(std::holds_alternative<sn::ErrorOutputTruncated>(*truncated));
    CHECK_EQ(std::get<sn::ErrorOutputTruncated>(*truncated).provider, std::string("OpenAI"));
    CHECK_EQ(std::get<sn::ErrorOutputTruncated>(*truncated).stop_reason.value_or(""), std::string("length"));

    termination = sn::StreamTermination{};
    termination.produced_content = true;
    CHECK(!termination.is_retryable());
    termination = sn::StreamTermination{};
    termination.already_reported_error = true;
    CHECK(!termination.is_retryable());
    CHECK(!termination.to_error("OpenAI").has_value());

    termination = sn::StreamTermination{};
    termination.timed_out = true;
    CHECK(termination.is_retryable());
    const auto timed_out = termination.to_error("OpenAI");
    CHECK(timed_out.has_value() && std::holds_alternative<sn::ErrorTimeout>(*timed_out));

    termination = sn::StreamTermination{};
    termination.tool_call_in_flight = true;
    const auto incomplete = termination.to_error("OpenAI");
    CHECK(incomplete.has_value());
    CHECK(std::holds_alternative<sn::ErrorIncompleteStream>(*incomplete));
    CHECK(std::get<sn::ErrorIncompleteStream>(*incomplete).tool_call_in_flight);
    CHECK(!std::get<sn::ErrorIncompleteStream>(*incomplete).produced_content);

    // A stream error with a non-retryable detail only stays retryable via the explicit flag or
    // a known transient message.
    sn::StreamTermination stream_error_termination;
    stream_error_termination.saw_terminal_marker = true;
    stream_error_termination.stream_error = sn::GenerationError{
        sn::ErrorApi{std::nullopt, std::string("relay"), "invalid api key"}};
    stream_error_termination.retryable_stream_error = false;
    CHECK(!stream_error_termination.is_retryable());
    CHECK(stream_error_termination.to_error("OpenAI").has_value());
    stream_error_termination.stream_error = sn::GenerationError{
        sn::ErrorApi{std::nullopt, std::string("relay"), "failed to generate"}};
    CHECK(stream_error_termination.is_retryable());

    // A clean terminal marker with nothing else yields no error.
    sn::StreamTermination clean;
    clean.saw_terminal_marker = true;
    CHECK(!clean.to_error("OpenAI").has_value());
    CHECK(!clean.is_retryable());
}

TEST(stream_normalization_carries_model_output) {
    CHECK(sn::carries_model_output(sn::StreamEvent{sn::TextChunk{"x"}}));
    CHECK(!sn::carries_model_output(sn::StreamEvent{sn::TextChunk{""}}));
    CHECK(!sn::carries_model_output(sn::StreamEvent{sn::CitationUpdate{}}));
    CHECK(sn::carries_model_output(sn::StreamEvent{sn::ThoughtChunk{"x", std::nullopt, std::nullopt}}));
    CHECK(sn::carries_model_output(
        sn::StreamEvent{sn::ThoughtChunk{"", std::nullopt, std::string("sig")}}));
    CHECK(!sn::carries_model_output(sn::StreamEvent{sn::ThoughtChunk{"", std::nullopt, std::nullopt}}));
    CHECK(sn::carries_model_output(sn::StreamEvent{
        sn::HostedToolCallUpdate{"k", "n", "{}", std::nullopt, false}}));
    CHECK(sn::carries_model_output(sn::StreamEvent{
        sn::ToolCallUpdate{"k", std::nullopt, "n", "{}", std::nullopt}}));
    CHECK(sn::carries_model_output(sn::StreamEvent{
        sn::ToolCallRequest{"id", "n", "{}", std::nullopt, "id", 0}}));
    CHECK(sn::carries_model_output(sn::StreamEvent{sn::ToolCallsRequest{}}));
    CHECK(!sn::carries_model_output(sn::StreamEvent{sn::UsageUpdate{1, std::nullopt}}));
    CHECK(!sn::carries_model_output(sn::StreamEvent{sn::Retrying{1, 5}}));
    CHECK(!sn::carries_model_output(
        sn::StreamEvent{sn::ErrorEvent{sn::ErrorTimeout{}}}));
}

TEST(stream_normalization_accumulator_rules) {
    sn::ToolArgumentAccumulator accumulator;
    CHECK(accumulator.is_empty());
    accumulator.append(std::nullopt);
    accumulator.append(std::string_view{});
    CHECK(accumulator.is_empty());

    accumulator.append(std::string_view{"{\"a\""});
    accumulator.append(std::string_view{":1}"});
    CHECK_EQ(accumulator.str(), std::string("{\"a\":1}"));

    // A growing snapshot replaces instead of concatenating.
    sn::ToolArgumentAccumulator snapshot;
    snapshot.append(std::string_view{"{\"a\":1"});
    snapshot.append(std::string_view{"{\"a\":1,\"b\":2}"});
    CHECK_EQ(snapshot.str(), std::string("{\"a\":1,\"b\":2}"));
    // An exact resend is ignored (ambiguous single characters could not reach here).
    snapshot.append(std::string_view{"{\"a\":1,\"b\":2}"});
    CHECK_EQ(snapshot.str(), std::string("{\"a\":1,\"b\":2}"));

    // A substantial stale snapshot is ignored.
    sn::ToolArgumentAccumulator stale;
    stale.append(std::string_view{"{\"long_key\":\"long_value\"}"});
    stale.append(std::string_view{"{\"long_key\":\"long"});
    CHECK_EQ(stale.str(), std::string("{\"long_key\":\"long_value\"}"));

    // A short repeated head is a normal increment (the `[` + `{"` chunk pair).
    sn::ToolArgumentAccumulator increments;
    increments.append(std::string_view{"[{\"a\":1},"});
    increments.append(std::string_view{"{\"b\"}]"});
    CHECK_EQ(increments.str(), std::string("[{\"a\":1},{\"b\"}]"));

    sn::ToolArgumentAccumulator replaced;
    replaced.append(std::string_view{"old"});
    replaced.replace("new");
    CHECK_EQ(replaced.str(), std::string("new"));
    CHECK(!replaced.is_empty());
}

TEST(stream_normalization_malformed_standin) {
    const sn::UuidFn uuids = fixed_uuid("11111111-2222-4333-8444-000000000000");
    const sn::ToolCallRequest request =
        sn::malformed_tool_call_request("bad call", std::nullopt, std::nullopt, std::nullopt,
                                        std::nullopt, uuids);
    CHECK_EQ(request.name, std::string("agora_malformed_tool_call"));
    CHECK_EQ(request.id, std::string("call_malformed_11111111222243338444000000000000"));
    CHECK_EQ(request.stream_key, std::string("malformed_tool_11111111222243338444000000000000"));
    CHECK_EQ(request.arguments, std::string("{\"error\":\"bad call\"}"));

    // A non-blank stream key survives; a blank one falls back to the compact suffix.
    const sn::ToolCallRequest keyed = sn::malformed_tool_call_request(
        "c", std::nullopt, std::nullopt, std::string("text_tool_x"), std::nullopt, uuids);
    CHECK_EQ(keyed.stream_key, std::string("text_tool_x"));
    const sn::ToolCallRequest blank = sn::malformed_tool_call_request(
        "c", std::nullopt, std::nullopt, std::string("   "), std::nullopt, uuids);
    CHECK(blank.stream_key.rfind("malformed_tool_", 0) == 0);

    // Original name/arguments are kept verbatim but capped at 200 / 2000 UTF-16 units.
    const std::string long_name(250, 'a');
    const std::string long_arguments(2100, 'b');
    const sn::ToolCallRequest capped = sn::malformed_tool_call_request(
        "c", long_name, long_arguments, std::nullopt, std::nullopt, uuids);
    CHECK(capped.arguments.find(std::string(200, 'a')) != std::string::npos);
    CHECK(capped.arguments.find(std::string(201, 'a')) == std::string::npos);
    CHECK(capped.arguments.find(std::string(2000, 'b')) != std::string::npos);
    CHECK(capped.arguments.find(std::string(2001, 'b')) == std::string::npos);

    // The cause is escaped through the pinned kotlinx escaping.
    const sn::ToolCallRequest escaped = sn::malformed_tool_call_request(
        "quote \" backslash \\\n", std::nullopt, std::nullopt, std::nullopt, std::nullopt, uuids);
    CHECK_EQ(escaped.arguments,
             std::string("{\"error\":\"quote \\\" backslash \\\\\\n\"}"));
}

TEST(stream_normalization_malformed_result_text) {
    const sn::ToolCallRequest request = sn::malformed_tool_call_request(
        "cause text", std::string("tool name"), std::string("{\"a\":1}"), std::nullopt,
        std::nullopt, counting_uuids());
    const std::string result = sn::malformed_tool_call_result_text(request.arguments);
    CHECK_EQ(result,
             std::string("Error: the previous tool call was malformed and was not executed"
                         ". Cause: cause text"
                         ". Tool name as sent: tool name"
                         ". Arguments as sent: {\"a\":1}"
                         ". Re-issue the call with one of the offered tool names and a complete "
                         "JSON object as arguments. If the arguments were cut off, send a smaller "
                         "payload."));

    CHECK_EQ(sn::malformed_tool_call_result_text("not json"),
             std::string("Error: the previous tool call was malformed and was not executed"
                         ". Re-issue the call with one of the offered tool names and a complete "
                         "JSON object as arguments. If the arguments were cut off, send a smaller "
                         "payload."));

    // JsonNull fields surface as the literal "null" (JsonNull is a JsonPrimitive).
    CHECK_EQ(sn::malformed_tool_call_result_text(
                 "{\"error\":null,\"original_name\":\"n\"}"),
             std::string("Error: the previous tool call was malformed and was not executed"
                         ". Cause: null"
                         ". Tool name as sent: n"
                         ". Re-issue the call with one of the offered tool names and a complete "
                         "JSON object as arguments. If the arguments were cut off, send a smaller "
                         "payload."));
}

TEST(stream_normalization_text_tool_ids_and_parsers) {
    // Injected UUIDs surface as valid wire ids on completed text tool calls.
    sn::StreamingTextToolCallParser parser(counting_uuids());
    std::vector<sn::StreamingTextToolCallParser::CompletedCall> completed;
    sn::StreamingTextToolCallParser::Callbacks callbacks;
    callbacks.on_text = [](const std::string&) {};
    callbacks.on_update = [](const sn::StreamingTextToolCallParser::Snapshot&) {};
    callbacks.on_complete = [&](const sn::StreamingTextToolCallParser::CompletedCall& call) {
        completed.push_back(call);
    };
    callbacks.on_malformed = [](const std::string&) {};
    parser.feed(std::string(kOpenToolCall) + "{\"name\":\"t\",\"arguments\":{\"a\":1}}" +
                    std::string(kCloseToolCall),
                callbacks);
    parser.flush(callbacks);
    CHECK_EQ(completed.size(), std::size_t{1});
    CHECK(completed[0].stream_key.rfind("text_tool_", 0) == 0);
    CHECK(!completed[0].id.has_value());
    CHECK_EQ(completed[0].name, std::string("t"));
    CHECK_EQ(completed[0].arguments, std::string("{\"a\":1}"));

    // ToolCallTextParser.parse: XML invoke with the antml: prefix, id attribute and parameters.
    const std::string xml_content =
        "<antml:" "invoke name=\"run\" id=\"call-1\">"
        "<antml:" "parameter name=\"command\">ls</antml:" "parameter>"
        "</antml:" "invoke>";
    const std::vector<sn::ParsedTextCall> xml_calls = sn::parse_tool_call_text(xml_content);
    CHECK_EQ(xml_calls.size(), std::size_t{1});
    CHECK_EQ(xml_calls[0].id.value_or(""), std::string("call-1"));
    CHECK_EQ(xml_calls[0].name, std::string("run"));
    CHECK_EQ(xml_calls[0].arguments, std::string("{\"command\":\"ls\"}"));

    // An invalid id attribute poisons the whole block.
    const std::string bad_id =
        "<invoke name=\"run\" id=\"bad id\">"
        "<parameter name=\"a\">1</parameter>"
        "</invoke>";
    CHECK(sn::parse_tool_call_text(bad_id).empty());

    // A damaged tagged block rejects the whole payload (all-or-nothing).
    const std::string damaged =
        std::string(kOpenToolCall) + "{\"name\":\"a\",\"arguments\":{}}" + std::string(kCloseToolCall) +
        std::string(kOpenToolCall) + "not json" + std::string(kCloseToolCall);
    CHECK(sn::parse_tool_call_text(damaged).empty());
}

TEST(stream_normalization_normalizer_gates) {
    // Authoritative native parsing suppresses text tool recovery entirely.
    {
        sn::ProviderStreamNormalizer normalizer({"t"}, true, counting_uuids());
        std::vector<sn::StreamEvent> events;
        const sn::ProviderStreamNormalizer::Downstream downstream =
            [&](const sn::StreamEvent& event) { events.push_back(event); };
        const std::string tagged =
            std::string(kOpenToolCall) + "{\"name\":\"t\",\"arguments\":{}}" +
            std::string(kCloseToolCall);
        normalizer.emit(sn::StreamEvent{sn::TextChunk{tagged}}, downstream);
        normalizer.finish(downstream);
        CHECK_EQ(events.size(), std::size_t{1});
        CHECK(std::holds_alternative<sn::TextChunk>(events[0]));
        CHECK_EQ(std::get<sn::TextChunk>(events[0]).text, tagged);
    }

    // Without tools there is no parser either: text passes through untouched.
    {
        sn::ProviderStreamNormalizer normalizer({}, false, counting_uuids());
        std::vector<sn::StreamEvent> events;
        const sn::ProviderStreamNormalizer::Downstream downstream =
            [&](const sn::StreamEvent& event) { events.push_back(event); };
        normalizer.emit(sn::StreamEvent{sn::TextChunk{"hello"}}, downstream);
        normalizer.finish(downstream);
        CHECK_EQ(events.size(), std::size_t{1});
        CHECK_EQ(std::get<sn::TextChunk>(events[0]).text, std::string("hello"));
    }

    // A complete tagged block is held back until finish, then released as one tool call.
    {
        sn::ProviderStreamNormalizer normalizer({"t"}, false, counting_uuids());
        std::vector<sn::StreamEvent> events;
        const sn::ProviderStreamNormalizer::Downstream downstream =
            [&](const sn::StreamEvent& event) { events.push_back(event); };
        const std::string tagged =
            std::string(kOpenToolCall) + "{\"name\":\"t\",\"arguments\":{\"a\":1}}" +
            std::string(kCloseToolCall);
        normalizer.emit(sn::StreamEvent{sn::TextChunk{tagged}}, downstream);
        CHECK(events.empty());
        normalizer.finish(downstream);
        CHECK_EQ(events.size(), std::size_t{1});
        CHECK(std::holds_alternative<sn::ToolCallRequest>(events[0]));
        const sn::ToolCallRequest& call = std::get<sn::ToolCallRequest>(events[0]);
        CHECK(is_safe_call_id(call.id));
        CHECK(call.id.rfind("call_text_", 0) == 0);
        CHECK_EQ(call.name, std::string("t"));
        CHECK_EQ(call.arguments, std::string("{\"a\":1}"));
        CHECK(call.stream_key.rfind("text_tool_", 0) == 0);
    }

    // The tool probe is anchored: the same block after ordinary prose is not a tool call.
    {
        sn::ProviderStreamNormalizer normalizer({"t"}, false, counting_uuids());
        std::vector<sn::StreamEvent> events;
        const sn::ProviderStreamNormalizer::Downstream downstream =
            [&](const sn::StreamEvent& event) { events.push_back(event); };
        const std::string tagged =
            std::string(kOpenToolCall) + "{\"name\":\"t\",\"arguments\":{}}" +
            std::string(kCloseToolCall);
        const std::string content = "before " + tagged + " after";
        normalizer.emit(sn::StreamEvent{sn::TextChunk{content}}, downstream);
        normalizer.finish(downstream);
        CHECK_EQ(events.size(), std::size_t{1});
        CHECK(std::holds_alternative<sn::TextChunk>(events[0]));
        CHECK_EQ(std::get<sn::TextChunk>(events[0]).text, content);
    }

    // Emitting after finish is the Kotlin check() contract.
    {
        sn::ProviderStreamNormalizer normalizer({"t"}, false, counting_uuids());
        const sn::ProviderStreamNormalizer::Downstream downstream = [](const sn::StreamEvent&) {};
        normalizer.finish(downstream);
        bool thrown = false;
        try {
            normalizer.emit(sn::StreamEvent{sn::TextChunk{"x"}}, downstream);
        } catch (const sn::IllegalStateError&) {
            thrown = true;
        }
        CHECK(thrown);
        // finish is idempotent.
        normalizer.finish(downstream);
        normalizer.finish(downstream);
    }
}
