// Stream-normalization support types: UUID plumbing, ToolArgumentAccumulator, ProviderRetryPolicy,
// StreamTermination, carriesModelOutput and the malformed-call stand-in.

#include <algorithm>
#include <random>
#include <string>
#include <utility>

#include "agora/core/stream_normalization.hpp"
#include "stream_normalization_internal.hpp"

namespace agora::core::stream_normalization {

namespace json = agora::core::json;

// ---------------------------------------------------------------------------------------------
// UUID plumbing (mirrors java.util.UUID.randomUUID().toString()).

std::string random_uuid_v4() {
    static constexpr char kHex[] = "0123456789abcdef";
    static thread_local std::mt19937_64 engine(std::random_device{}());
    std::uniform_int_distribution<int> nibble(0, 15);
    std::string out;
    out.reserve(36);
    for (int position = 0; position < 36; ++position) {
        if (position == 8 || position == 13 || position == 18 || position == 23) {
            out.push_back('-');
        } else if (position == 14) {
            out.push_back('4');
        } else if (position == 19) {
            out.push_back(kHex[8 + (nibble(engine) & 3)]);
        } else {
            out.push_back(kHex[nibble(engine)]);
        }
    }
    return out;
}

std::string compact_uuid(std::string_view dashed) {
    std::string out;
    out.reserve(dashed.size());
    for (char c : dashed) {
        if (c != '-') out.push_back(c);
    }
    return out;
}

// ---------------------------------------------------------------------------------------------
// ToolArgumentAccumulator.

ToolArgumentAccumulator::ToolArgumentAccumulator(std::string initial) : builder_(std::move(initial)) {}

bool ToolArgumentAccumulator::is_empty() const {
    return builder_.empty();
}

void ToolArgumentAccumulator::append(std::optional<std::string_view> fragment) {
    if (!fragment.has_value() || fragment->empty()) return;
    if (builder_.empty()) {
        builder_ += *fragment;
        return;
    }
    // A single character is ambiguous (a lone `{` is a plausible increment), so snapshot
    // detection requires enough overlap to actually mean something.
    const std::size_t current_length = json::utf16_length(builder_);
    const std::size_t fragment_length = json::utf16_length(*fragment);
    if (current_length >= 2 && fragment_length >= 2) {
        if (*fragment == builder_) return;
        if (fragment_length > current_length && fragment->compare(0, builder_.size(), builder_) == 0) {
            builder_.assign(*fragment);
            return;
        }
        // Only a substantial resend that covers most of the accumulated value is a stale
        // snapshot; discarding an increment cannot be undone later in the stream.
        if (fragment_length >= 16 && fragment_length * 2 >= current_length &&
            builder_.compare(0, fragment->size(), *fragment) == 0) {
            return;
        }
    }
    builder_ += *fragment;
}

void ToolArgumentAccumulator::replace(std::string value) {
    builder_ = std::move(value);
}

const std::string& ToolArgumentAccumulator::str() const {
    return builder_;
}

// ---------------------------------------------------------------------------------------------
// ProviderRetryPolicy.

std::int64_t ProviderRetryPolicy::delay_millis(int retry_number) {
    if (retry_number >= 1 && retry_number <= 3) return 5'000;
    if (retry_number >= 4 && retry_number <= 5) return 30'000;
    throw IllegalStateError("Invalid retry number: " + std::to_string(retry_number));
}

bool ProviderRetryPolicy::is_failed_to_generate_outcome(const std::optional<std::string>& raw) {
    return raw.has_value() && detail::contains_ci(*raw, "failed to generate");
}

bool ProviderRetryPolicy::is_unreadable_server_response_failure(const std::optional<std::string>& raw) {
    return raw.has_value() && detail::contains_ci(*raw, "the server response could not be read");
}

bool ProviderRetryPolicy::is_retryable_upstream_failure(const std::optional<std::string>& raw) {
    if (!raw.has_value()) return false;
    return detail::contains_ci(*raw, "failed to generate") ||
        detail::contains_ci(*raw, "the server response could not be read");
}

bool ProviderRetryPolicy::should_retry_http(
    int status_code,
    const std::optional<std::string>& body,
    const std::set<int>& retryable_status_codes) {
    if (retryable_status_codes.count(status_code) != 0) return true;
    return is_retryable_upstream_failure(body);
}

bool ProviderRetryPolicy::should_retry_stream_error(const GenerationError* error) {
    if (error == nullptr) return false;
    if (const auto* api = std::get_if<ErrorApi>(error)) {
        return is_retryable_upstream_failure(api->message);
    }
    if (const auto* network = std::get_if<ErrorNetwork>(error)) {
        return is_retryable_upstream_failure(network->message);
    }
    return false;
}

// ---------------------------------------------------------------------------------------------
// StreamTermination.

namespace {

bool is_token_cap_reason(const std::optional<std::string>& stop_reason) {
    if (!stop_reason.has_value()) return false;
    return *stop_reason == "max_tokens" || *stop_reason == "length" ||
        *stop_reason == "max_output_tokens";
}

const char* bool_text(bool value) {
    return value ? "true" : "false";
}

}  // namespace

bool StreamTermination::truncated_by_token_cap() const {
    return is_token_cap_reason(stop_reason);
}

bool StreamTermination::is_retryable() const {
    return !produced_content && !already_reported_error && !truncated_by_token_cap() &&
        (timed_out ||
            (stream_error.has_value() &&
                (retryable_stream_error || ProviderRetryPolicy::should_retry_stream_error(
                    stream_error.has_value() ? &*stream_error : nullptr))) ||
            tool_call_in_flight || !saw_terminal_marker);
}

std::optional<GenerationError> StreamTermination::to_error(const std::string& provider) const {
    if (already_reported_error) return std::nullopt;
    if (stream_error.has_value()) return stream_error;
    if (timed_out) return GenerationError{ErrorTimeout{}};
    if (truncated_by_token_cap()) return GenerationError{ErrorOutputTruncated{provider, stop_reason}};
    if (tool_call_in_flight || !saw_terminal_marker) {
        return GenerationError{ErrorIncompleteStream{
            provider,
            stop_reason,
            tool_call_in_flight,
            produced_content,
        }};
    }
    return std::nullopt;
}

std::string StreamTermination::describe() const {
    std::string out = "stop_reason=";
    out += stop_reason.has_value() ? *stop_reason : "null";
    out += " terminal_marker=";
    out += bool_text(saw_terminal_marker);
    out += " produced_content=";
    out += bool_text(produced_content);
    out += " tool_in_flight=";
    out += bool_text(tool_call_in_flight);
    out += " timed_out=";
    out += bool_text(timed_out);
    out += " stream_error=";
    out += bool_text(stream_error.has_value());
    out += " reported_error=";
    out += bool_text(already_reported_error);
    out += " retryable=";
    out += bool_text(is_retryable());
    return out;
}

bool carries_model_output(const StreamEvent& event) {
    struct Visitor {
        bool operator()(const TextChunk& chunk) const { return !chunk.text.empty(); }
        bool operator()(const CitationUpdate&) const { return false; }
        bool operator()(const ThoughtChunk& chunk) const {
            return !chunk.thought.empty() || chunk.signature.has_value();
        }
        bool operator()(const HostedToolCallUpdate&) const { return true; }
        bool operator()(const ToolCallUpdate&) const { return true; }
        bool operator()(const ToolCallRequest&) const { return true; }
        bool operator()(const ToolCallsRequest&) const { return true; }
        bool operator()(const UsageUpdate&) const { return false; }
        bool operator()(const Retrying&) const { return false; }
        bool operator()(const ErrorEvent&) const { return false; }
    };
    return std::visit(Visitor{}, event);
}

// ---------------------------------------------------------------------------------------------
// MalformedToolCall.

const std::string_view kMalformedToolCallName = "agora_malformed_tool_call";

namespace {

constexpr std::size_t kOriginalNameMaxChars = 200;
constexpr std::size_t kOriginalArgumentsMaxChars = 2'000;

/** kotlinx JsonObject.get(key): the last duplicate value wins. */
const json::Value* member_or_null(const json::Value& object, std::string_view key) {
    if (object.kind != json::Kind::Object) return nullptr;
    const json::Value* found = nullptr;
    for (const auto& member : object.members) {
        if (member.key == key) found = &member.value;
    }
    return found;
}

void append_json_string(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

}  // namespace

ToolCallRequest malformed_tool_call_request(
    const std::string& cause,
    std::optional<std::string> original_name,
    std::optional<std::string> original_arguments,
    std::optional<std::string> stream_key,
    std::optional<std::string> signature,
    UuidFn uuid) {
    const std::string suffix = compact_uuid(uuid ? uuid() : random_uuid_v4());

    std::string arguments = "{";
    arguments += "\"error\":";
    append_json_string(arguments, cause);
    if (original_name.has_value() && !original_name->empty()) {
        arguments += ",\"original_name\":";
        append_json_string(arguments, json::take_utf16_units(*original_name, kOriginalNameMaxChars));
    }
    if (original_arguments.has_value() && !original_arguments->empty()) {
        arguments += ",\"original_arguments\":";
        append_json_string(
            arguments,
            json::take_utf16_units(*original_arguments, kOriginalArgumentsMaxChars));
    }
    arguments += "}";

    ToolCallRequest request;
    request.id = "call_malformed_" + suffix;
    request.name = std::string(kMalformedToolCallName);
    request.arguments = std::move(arguments);
    request.signature = std::move(signature);
    if (stream_key.has_value() && !json::is_blank(*stream_key)) {
        request.stream_key = std::move(*stream_key);
    } else {
        request.stream_key = "malformed_tool_" + suffix;
    }
    return request;
}

std::string malformed_tool_call_result_text(std::string_view arguments) {
    const json::ParseResult parsed = json::parse(arguments);
    const auto field = [&](std::string_view key) -> std::optional<std::string> {
        if (!parsed.value.has_value()) return std::nullopt;
        const json::Value* found = member_or_null(*parsed.value, key);
        if (found == nullptr) return std::nullopt;
        // JsonPrimitive covers both strings and literals (JsonNull included: content "null").
        if (found->kind == json::Kind::String || found->kind == json::Kind::Literal) {
            return found->text;
        }
        return std::nullopt;
    };

    std::string out = "Error: the previous tool call was malformed and was not executed";
    if (const auto error = field("error")) {
        out += ". Cause: ";
        out += *error;
    }
    if (const auto name = field("original_name")) {
        out += ". Tool name as sent: ";
        out += *name;
    }
    if (const auto sent = field("original_arguments")) {
        out += ". Arguments as sent: ";
        out += *sent;
    }
    out += ". Re-issue the call with one of the offered tool names and a complete JSON object " +
        std::string("as arguments. If the arguments were cut off, send a smaller payload.");
    return out;
}

}  // namespace agora::core::stream_normalization
