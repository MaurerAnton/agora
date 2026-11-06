// Stream-normalization support types: UUID plumbing, ToolArgumentAccumulator, ProviderRetryPolicy,
// StreamTermination, carriesModelOutput and the malformed-call stand-in.

#include <algorithm>
#include <cstdio>
#include <limits>
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

namespace {

/** NetworkRetryPolicy.HTTP_CODES. */
bool is_transient_http_code(int status_code) {
    return status_code == 408 || status_code == 425 || status_code == 429 || status_code == 500 ||
        status_code == 502 || status_code == 503 || status_code == 504 || status_code == 529;
}

bool is_client_error_status(int status_code) {
    return status_code >= 400 && status_code <= 499;
}

std::string trim_copy(std::string value) {
    const auto first = value.find_first_not_of(" \t\n\r");
    if (first == std::string::npos) return {};
    const auto last = value.find_last_not_of(" \t\n\r");
    return value.substr(first, last - first + 1);
}

std::string lower_ascii(std::string value) {
    for (char& c : value) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return value;
}

const std::vector<std::string>& retryable_upstream_messages() {
    static const std::vector<std::string> kMessages = {
        "failed to generate",
        "the server response could not be read",
        "rate limit",
        "rate_limit",
        "too many requests",
        "overloaded",
        "temporarily unavailable",
    };
    return kMessages;
}

const std::set<std::string>& permanent_error_identifiers() {
    static const std::set<std::string> kIdentifiers = {
        "insufficient_quota", "quota_exhausted", "exhausted_quota",
        "insufficient_balance", "insufficient_funds", "insufficient_credit", "insufficient_credits",
        "credit_balance_too_low", "credits_exhausted", "billing_hard_limit_reached", "billing_not_active",
        "usage_limit_reached", "spend_limit_reached",
        "invalid_api_key", "authentication_error", "unauthenticated", "permission_denied",
        "permission_error", "unauthorized", "forbidden", "401", "403",
        "invalid_request", "invalid_request_error", "invalid_argument", "bad_request",
        "context_length_exceeded", "model_not_found",
        "unsupported_parameter", "unsupported_value", "content_policy_violation",
        "400", "404", "422",
    };
    return kIdentifiers;
}

const std::vector<std::string>& permanent_error_messages() {
    static const std::vector<std::string> kMessages = {
        "insufficient quota", "quota exhausted", "quota has been exhausted",
        "exceeded your current quota", "exceeded your monthly quota",
        "insufficient balance", "insufficient funds", "insufficient credit",
        "credit balance is too low", "credit balance too low", "credits exhausted",
        "out of credits", "billing hard limit", "billing limit reached",
        "billing is not active", "spending limit reached", "spend limit reached",
        "invalid api key", "incorrect api key", "invalid authentication",
        "authentication failed", "permission denied", "invalid request",
        "unsupported parameter", "unsupported value", "context length exceeded",
        "maximum context length", "model not found", "content policy violation",
    };
    return kMessages;
}

const std::set<std::string>& retryable_error_identifiers() {
    static const std::set<std::string> kIdentifiers = {
        "rate_limit_error", "rate_limit_exceeded", "too_many_requests", "overloaded_error",
        "server_error", "internal_error", "internal_server_error", "unavailable",
        "resource_exhausted", "failed_to_generate", "408", "425", "429", "500", "502", "503",
        "504", "529",
    };
    return kIdentifiers;
}

const std::vector<std::string>& transient_messages() {
    static const std::vector<std::string> kMessages = {
        "unexpected end of stream", "unexpected end of file", "eofexception",
        "software caused connection abort", "socket is closed", "socket closed",
        "connection reset", "connection aborted", "broken pipe", "connection shutdown",
        "ssl peer shut down incorrectly", "connection closed by peer", "unexpected eof",
        "stream was reset", "protocol_error", "refused_stream", "timed out", "timeout",
        "the server response could not be read", "incomplete encrypted event line",
    };
    return kMessages;
}

bool matches_any_ci(const std::optional<std::string>& text, const std::vector<std::string>& needles) {
    if (!text.has_value()) return false;
    for (const std::string& needle : needles) {
        if (detail::contains_ci(*text, needle)) return true;
    }
    return false;
}

/** providerHttpError(status, body) for the plain-text bodies the traces carry. */
struct HttpErrorParts {
    std::optional<std::string> code;
    std::optional<std::string> type;
    std::string message;
};

HttpErrorParts provider_http_error_parts(int status_code, const std::optional<std::string>& body) {
    const std::string trimmed = body.has_value() ? trim_copy(*body) : std::string{};
    if (trimmed.empty()) {
        return HttpErrorParts{std::nullopt, std::nullopt, "HTTP " + std::to_string(status_code)};
    }
    // The Kotlin reference parses a JSON body into code/type/message. Every pinned trace carries
    // plain text, which the parser passes through verbatim with the status as the code.
    return HttpErrorParts{std::to_string(status_code), std::nullopt, trimmed};
}

std::int64_t days_from_civil(int year, unsigned month, unsigned day) {
    year -= month <= 2;
    const int era = (year >= 0 ? year : year - 399) / 400;
    const unsigned yoe = static_cast<unsigned>(year - era * 400);
    const unsigned doy = (153 * (month + (month > 2 ? -3 : 9)) + 2) / 5 + day - 1;
    const unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return static_cast<std::int64_t>(era) * 146097 + static_cast<std::int64_t>(doe) - 719468;
}

std::optional<std::int64_t> parse_http_date_millis(const std::string& raw) {
    // RFC 1123: "Wed, 21 Oct 2015 07:28:00 GMT".
    const auto comma = raw.find(',');
    if (comma == std::string::npos) return std::nullopt;
    int day = 0;
    int year = 0;
    int hour = 0;
    int minute = 0;
    int second = 0;
    char month[4] = {0, 0, 0, 0};
    char zone[4] = {0, 0, 0, 0};
    const std::string rest = trim_copy(raw.substr(comma + 1));
    if (std::sscanf(rest.c_str(), "%d %3s %d %d:%d:%d %3s", &day, month, &year, &hour, &minute,
                    &second, zone) != 7) {
        return std::nullopt;
    }
    static const char* kMonths[] = {"Jan", "Feb", "Mar", "Apr", "May", "Jun",
                                    "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"};
    int month_index = -1;
    for (int i = 0; i < 12; ++i) {
        if (lower_ascii(month) == lower_ascii(kMonths[i])) month_index = i;
    }
    if (month_index < 0 || lower_ascii(zone) != "gmt" || day < 1 || day > 31 || hour < 0 ||
        hour > 23 || minute < 0 || minute > 59 || second < 0 || second > 60) {
        return std::nullopt;
    }
    const std::int64_t days = days_from_civil(year, static_cast<unsigned>(month_index + 1),
                                              static_cast<unsigned>(day));
    const std::int64_t seconds = days * 86400 + static_cast<std::int64_t>(hour) * 3600 +
        static_cast<std::int64_t>(minute) * 60 + second;
    return seconds * 1000;
}

std::optional<std::int64_t> parse_decimal_long(const std::string& raw) {
    if (raw.empty()) return std::nullopt;
    std::size_t index = 0;
    bool negative = false;
    if (raw[index] == '+' || raw[index] == '-') {
        negative = raw[index] == '-';
        ++index;
    }
    if (index == raw.size()) return std::nullopt;
    std::int64_t value = 0;
    for (; index < raw.size(); ++index) {
        const char c = raw[index];
        if (c < '0' || c > '9') return std::nullopt;
        if (value > (std::numeric_limits<std::int64_t>::max() - (c - '0')) / 10) {
            return std::nullopt;
        }
        value = value * 10 + (c - '0');
    }
    return negative ? -value : value;
}

}  // namespace

std::int64_t ProviderRetryPolicy::delay_millis(int retry_number,
                                               const std::optional<std::string>& retry_after,
                                               std::int64_t now_millis,
                                               const JitterSource& jitter) {
    if (retry_number < 1 || retry_number > kMaxRetries) {
        throw IllegalArgumentError("Invalid retry number: " + std::to_string(retry_number));
    }
    // Equal jitter: 0.5–1, 1–2, 2–4, 4–8, 8–16 seconds.
    const std::int64_t ceiling = 1'000LL << (retry_number - 1);
    std::int64_t backoff = 0;
    if (jitter) {
        backoff = jitter(ceiling / 2, ceiling + 1);
    } else {
        static thread_local std::mt19937_64 engine([] {
            std::random_device device;
            return (static_cast<std::uint64_t>(device()) << 32) ^ device();
        }());
        std::uniform_int_distribution<std::int64_t> distribution(ceiling / 2, ceiling);
        backoff = distribution(engine);
    }
    const std::int64_t retry_after_value = retry_after_millis(retry_after, now_millis).value_or(0);
    return std::max(backoff, retry_after_value);
}

std::optional<std::int64_t> ProviderRetryPolicy::retry_after_millis(
    const std::optional<std::string>& value, std::int64_t now_millis) {
    if (!value.has_value()) return std::nullopt;
    const std::string raw = trim_copy(*value);
    if (raw.empty()) return std::nullopt;
    if (const std::optional<std::int64_t> seconds = parse_decimal_long(raw); seconds.has_value()) {
        const std::int64_t limit = std::numeric_limits<std::int64_t>::max() / 1'000;
        const std::int64_t clamped = std::max<std::int64_t>(0, std::min(*seconds, limit));
        return clamped * 1'000;
    }
    const std::optional<std::int64_t> epoch = parse_http_date_millis(raw);
    if (!epoch.has_value()) return std::nullopt;
    return std::max<std::int64_t>(0, *epoch - now_millis);
}

bool ProviderRetryPolicy::is_failed_to_generate_outcome(const std::optional<std::string>& raw) {
    return raw.has_value() && detail::contains_ci(*raw, "failed to generate");
}

bool ProviderRetryPolicy::is_transient_message(const std::optional<std::string>& message) {
    return matches_any_ci(message, transient_messages());
}

bool ProviderRetryPolicy::is_unreadable_server_response_failure(
    const std::optional<std::string>& raw) {
    return raw.has_value() && detail::contains_ci(*raw, "the server response could not be read");
}

bool ProviderRetryPolicy::is_retryable_upstream_failure(const std::optional<std::string>& raw) {
    return matches_any_ci(raw, retryable_upstream_messages());
}

bool ProviderRetryPolicy::is_permanent_api_failure(const std::optional<std::string>& code,
                                                   const std::optional<std::string>& type,
                                                   const std::optional<std::string>& message) {
    const std::set<std::string>& identifiers = permanent_error_identifiers();
    for (const std::optional<std::string>* candidate : {&code, &type}) {
        if (!candidate->has_value()) continue;
        if (identifiers.count(lower_ascii(trim_copy(**candidate))) != 0) return true;
    }
    if (message.has_value()) {
        for (const std::string& needle : permanent_error_messages()) {
            if (detail::contains_ci(*message, needle)) return true;
        }
    }
    return false;
}

bool ProviderRetryPolicy::is_permanent_http_failure(int status_code,
                                                    const std::optional<std::string>& body) {
    const bool permanent_status = is_client_error_status(status_code) &&
        !is_transient_http_code(status_code);
    if (permanent_status) return true;
    const HttpErrorParts parts = provider_http_error_parts(status_code, body);
    return is_permanent_api_failure(parts.code, parts.type, parts.message);
}

bool ProviderRetryPolicy::is_response_read_http_failure(int status_code,
                                                        const std::optional<std::string>& body) {
    if (is_unreadable_server_response_failure(body)) return true;
    const HttpErrorParts parts = provider_http_error_parts(status_code, body);
    const bool read_failure = is_unreadable_server_response_failure(parts.message) ||
        is_transient_message(parts.message);
    return read_failure && !is_permanent_api_failure(parts.code, parts.type, parts.message);
}

bool ProviderRetryPolicy::requires_response_read_retry(const GenerationError* error) {
    if (error == nullptr) return false;
    if (std::holds_alternative<ErrorSseParse>(*error)) return true;
    if (std::holds_alternative<ErrorTimeout>(*error)) return true;
    if (const auto* api = std::get_if<ErrorApi>(error)) {
        const bool read_failure = is_unreadable_server_response_failure(api->message) ||
            is_transient_message(api->message);
        return read_failure && !is_permanent_api_failure(api->code, api->type, api->message);
    }
    if (const auto* network = std::get_if<ErrorNetwork>(error)) {
        const bool status_allows = !is_client_error_status(network->status_code) ||
            is_transient_http_code(network->status_code);
        return status_allows && (is_unreadable_server_response_failure(network->message) ||
                                 is_transient_message(network->message));
    }
    return false;
}

bool ProviderRetryPolicy::should_retry_http(
    int status_code,
    const std::optional<std::string>& body,
    const std::set<int>& retryable_status_codes) {
    if (is_permanent_http_failure(status_code, body)) return false;
    if (is_response_read_http_failure(status_code, body)) return true;
    if (is_transient_http_code(status_code)) return true;
    if (!is_client_error_status(status_code) && retryable_status_codes.count(status_code) != 0) {
        return true;
    }
    return !is_client_error_status(status_code) && is_retryable_upstream_failure(body);
}

bool ProviderRetryPolicy::should_retry_stream_error(const GenerationError* error) {
    if (error == nullptr) return false;
    if (requires_response_read_retry(error)) return true;
    if (const auto* api = std::get_if<ErrorApi>(error)) {
        if (is_permanent_api_failure(api->code, api->type, api->message)) return false;
        const std::set<std::string>& identifiers = retryable_error_identifiers();
        for (const std::optional<std::string>* candidate : {&api->code, &api->type}) {
            if (!candidate->has_value()) continue;
            if (identifiers.count(lower_ascii(trim_copy(**candidate))) != 0) return true;
        }
        return is_retryable_upstream_failure(api->message) || is_transient_message(api->message);
    }
    if (const auto* network = std::get_if<ErrorNetwork>(error)) {
        const bool status_allows = !is_client_error_status(network->status_code) ||
            is_transient_http_code(network->status_code);
        return status_allows && (is_transient_http_code(network->status_code) ||
                                 is_retryable_upstream_failure(network->message) ||
                                 is_transient_message(network->message));
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
    // Kotlin isRetryable: a response-read failure always consumes the retry budget (even when the
    // stream reported a token cap), a cap otherwise stays terminal, and produced content no longer
    // suppresses a replay that never proved its terminal marker. retryableStreamError is a legacy
    // constructor argument the reference reads nowhere.
    if (already_reported_error) return false;
    if (ProviderRetryPolicy::requires_response_read_retry(
            stream_error.has_value() ? &*stream_error : nullptr)) {
        return true;
    }
    if (truncated_by_token_cap()) return false;
    if (!stream_error.has_value()) {
        return timed_out || tool_call_in_flight || !saw_terminal_marker;
    }
    return ProviderRetryPolicy::should_retry_stream_error(&*stream_error);
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
