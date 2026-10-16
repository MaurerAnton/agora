#include "agora/core/openai_stream.hpp"

#include <cmath>
#include <cstdint>
#include <cstring>
#include <random>
#include <set>
#include <stdexcept>
#include <string>
#include <utility>

#include "agora/core/json.hpp"
#include "jvm_pow10_table.generated.hpp"

namespace agora::core::openai {
namespace {

// ---------------------------------------------------------------------------
// Small string helpers, pinned to Kotlin/JVM semantics.
// ---------------------------------------------------------------------------

char ascii_lower(char c) {
    return c >= 'A' && c <= 'Z' ? static_cast<char>(c - 'A' + 'a') : c;
}

bool equals_ignore_case(std::string_view a, std::string_view b) {
    if (a.size() != b.size()) return false;
    for (std::size_t i = 0; i < a.size(); ++i) {
        if (ascii_lower(a[i]) != ascii_lower(b[i])) return false;
    }
    return true;
}

/** Kotlin String.contains(other, ignoreCase = true) for provider ASCII tokens. */
bool contains_ignore_case(std::string_view haystack, std::string_view needle) {
    if (needle.empty()) return true;
    if (needle.size() > haystack.size()) return false;
    for (std::size_t i = 0; i + needle.size() <= haystack.size(); ++i) {
        if (equals_ignore_case(haystack.substr(i, needle.size()), needle)) return true;
    }
    return false;
}

/** Kotlin String.lowercase() restricted to the ASCII protocol spellings. */
std::string lowercase_ascii(std::string_view text) {
    std::string out;
    out.reserve(text.size());
    for (char c : text) out.push_back(ascii_lower(c));
    return out;
}

bool matches_safe_wire_name(std::string_view value) {
    if (value.empty() || value.size() > 64) return false;
    for (char c : value) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

bool matches_safe_wire_id(std::string_view value) {
    if (value.empty() || value.size() > 128) return false;
    for (char c : value) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                             (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) return false;
    }
    return true;
}

/** ProviderRetryPolicy.isFailedToGenerateOutcome. */
bool is_failed_to_generate(const std::optional<std::string>& outcome) {
    return outcome && contains_ignore_case(*outcome, "failed to generate");
}

/** ProviderRetryPolicy.shouldRetryStreamError. */
bool should_retry_stream_error(const Error& error) {
    if (error.kind == Error::Kind::Api || error.kind == Error::Kind::Network) {
        return contains_ignore_case(error.message, "failed to generate") ||
               contains_ignore_case(error.message, "the server response could not be read");
    }
    return false;
}

std::string random_uuid_v4() {
    static thread_local std::mt19937_64 engine([] {
        std::random_device device;
        const auto high = (static_cast<std::uint64_t>(device()) << 32) ^ device();
        const auto low = (static_cast<std::uint64_t>(device()) << 16) ^ device();
        return high ^ low;
    }());
    unsigned char bytes[16];
    const std::uint64_t first = engine();
    const std::uint64_t second = engine();
    for (int i = 0; i < 8; ++i) bytes[i] = (first >> (8 * (7 - i))) & 0xFF;
    for (int i = 0; i < 8; ++i) bytes[8 + i] = (second >> (8 * (7 - i))) & 0xFF;
    bytes[6] = static_cast<unsigned char>((bytes[6] & 0x0F) | 0x40);  // version 4
    bytes[8] = static_cast<unsigned char>((bytes[8] & 0x3F) | 0x80);  // variant
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(36);
    for (int i = 0; i < 16; ++i) {
        if (i == 4 || i == 6 || i == 8 || i == 10) out.push_back('-');
        out.push_back(kHex[bytes[i] >> 4]);
        out.push_back(kHex[bytes[i] & 0x0F]);
    }
    return out;
}

// ---------------------------------------------------------------------------
// consumeNumericLiteral: the exact kotlinx number decode, Long wrap included.
// ---------------------------------------------------------------------------

std::int64_t wrap_mul10_sub(std::int64_t value, int digit) {
    const std::uint64_t wrapped =
        static_cast<std::uint64_t>(value) * 10u - static_cast<std::uint64_t>(digit);
    return static_cast<std::int64_t>(wrapped);  // C++20: modulo 2^64, well defined
}

std::int64_t wrap_mul10_add(std::int64_t value, int digit) {
    const std::uint64_t wrapped =
        static_cast<std::uint64_t>(value) * 10u + static_cast<std::uint64_t>(digit);
    return static_cast<std::int64_t>(wrapped);
}

/**
 * 10.0.pow(exponent) with the exact bit patterns the desktop JVM's Math.pow
 * produces for the integer exponents the decode can reach, so the golden traces
 * stay reproducible on any host libm. Out-of-table arguments (|exponent| > 400
 * or non-integral) saturate to 0/Inf in every conforming implementation.
 */
double pow10_like_jvm(double exponent) {
    if (exponent >= -400.0 && exponent <= 400.0 && std::floor(exponent) == exponent) {
        const auto index = static_cast<int>(exponent) + 400;
        double result = 0.0;
        std::memcpy(&result, &kJvmPow10Bits[index], sizeof(result));
        return result;
    }
    return std::pow(10.0, exponent);
}

double calculate_exponent(std::int64_t exponent_accumulator, bool is_exponent_positive) {
    const double value = static_cast<double>(exponent_accumulator);
    return is_exponent_positive ? pow10_like_jvm(value) : pow10_like_jvm(-value);
}

/**
 * Reads a numeric literal starting at [start] in [source], following
 * AbstractJsonLexer.consumeNumericLiteral. Returns false on any rejection.
 */
bool decode_numeric_literal(std::string_view source, std::size_t start, std::int64_t& out) {
    std::size_t current = start;
    while (current < source.size()) {
        const char c = source[current];
        if (c == ' ' || c == '\n' || c == '\r' || c == '\t') {
            ++current;
        } else {
            break;
        }
    }
    if (current >= source.size()) return false;
    bool has_quotation = false;
    if (source[current] == '"') {
        ++current;
        if (current >= source.size()) return false;
        has_quotation = true;
    }
    std::int64_t accumulator = 0;
    std::int64_t exponent_accumulator = 0;
    bool is_negative = false;
    bool is_exponent_positive = false;
    bool has_exponent = false;
    const std::size_t literal_start = current;
    while (current < source.size()) {
        const char ch = source[current];
        if ((ch == 'e' || ch == 'E') && !has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = true;
            has_exponent = true;
            ++current;
            continue;
        }
        if (ch == '-' && has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = false;
            ++current;
            continue;
        }
        if (ch == '+' && has_exponent) {
            if (current == literal_start) return false;
            is_exponent_positive = true;
            ++current;
            continue;
        }
        if (ch == '-') {
            if (current != literal_start) return false;
            is_negative = true;
            ++current;
            continue;
        }
        if (!json::is_other_token_byte(static_cast<unsigned char>(ch))) break;
        ++current;
        const int digit = static_cast<unsigned char>(ch) - '0';
        if (digit < 0 || digit > 9) return false;
        if (has_exponent) {
            exponent_accumulator = wrap_mul10_add(exponent_accumulator, digit);
        } else {
            accumulator = wrap_mul10_sub(accumulator, digit);
            if (accumulator > 0) return false;  // a wrap past Long.MIN_VALUE
        }
    }
    const bool has_chars = current != literal_start;
    if (literal_start == current || (is_negative && literal_start == current - 1)) return false;
    if (has_quotation) {
        if (!has_chars) return false;
        if (current >= source.size() || source[current] != '"') return false;
        ++current;
    }
    if (has_exponent) {
        double double_accumulator =
            static_cast<double>(accumulator) * calculate_exponent(exponent_accumulator, is_exponent_positive);
        if (double_accumulator > 9223372036854775807.0) return false;
        if (double_accumulator < -9223372036854775808.0) return false;
        if (std::floor(double_accumulator) != double_accumulator) return false;
        accumulator = static_cast<std::int64_t>(double_accumulator);
    }
    if (is_negative) {
        out = accumulator;
        return true;
    }
    if (accumulator == INT64_MIN) return false;  // negating Long.MIN_VALUE overflows
    out = -accumulator;
    return true;
}

// ---------------------------------------------------------------------------
// The typed walk over the parsed document (decodeFromString<OpenAiStreamResponse>).
// ---------------------------------------------------------------------------

struct DecodeFailure : std::runtime_error {
    using std::runtime_error::runtime_error;
};

[[noreturn]] void fail(const std::string& message) {
    throw DecodeFailure(message);
}

const json::Value& require_object(const json::Value& value, const char* what) {
    if (value.kind != json::Kind::Object) fail(std::string(what) + " must be a JSON object");
    return value;
}

std::optional<std::string> decode_optional_string(const json::Value& value, const char* field) {
    if (value.is_literal("null")) return std::nullopt;
    if (value.kind != json::Kind::String) fail(std::string(field) + " must be a string");
    return value.text;
}

std::string decode_required_string(const json::Value& value, const char* field) {
    if (value.kind != json::Kind::String) fail(std::string(field) + " must be a string");
    return value.text;
}

std::int32_t int_gate(std::int64_t value, const char* field) {
    if (value != static_cast<std::int64_t>(static_cast<std::int32_t>(value))) {
        fail(std::string(field) + " does not fit an Int");
    }
    return static_cast<std::int32_t>(value);
}

std::optional<std::int32_t> decode_optional_int(std::string_view source, const json::Value& value,
                                                const char* field) {
    if (value.is_literal("null")) return std::nullopt;
    if (value.kind != json::Kind::Literal && value.kind != json::Kind::String) {
        fail(std::string(field) + " must be a number");
    }
    std::int64_t parsed = 0;
    if (!decode_numeric_literal(source, value.begin, parsed)) {
        fail(std::string(field) + " is not a valid number");
    }
    return int_gate(parsed, field);
}

std::int32_t decode_required_int(std::string_view source, const json::Value& value,
                                 const char* field) {
    // An explicit null cannot satisfy a required Int: the numeric scan hits the
    // 'n' run and rejects it, exactly like the reference decoder.
    if (value.kind != json::Kind::Literal && value.kind != json::Kind::String) {
        fail(std::string(field) + " must be a number");
    }
    std::int64_t parsed = 0;
    if (!decode_numeric_literal(source, value.begin, parsed)) {
        fail(std::string(field) + " is not a valid number");
    }
    return int_gate(parsed, field);
}

bool is_explicit_null(const json::Value& value) { return value.is_literal("null"); }

struct FunctionDelta {
    std::optional<std::string> name;
    bool arguments_present = false;
    const json::Value* arguments = nullptr;
};

struct ToolCallDelta {
    std::optional<std::int32_t> index;
    std::optional<std::string> id;
    std::optional<std::string> type;
    std::optional<FunctionDelta> function;
};

struct Delta {
    std::optional<std::string> role;
    std::optional<std::string> content;
    std::optional<std::string> reasoning_content;
    std::optional<std::string> reasoning;
    std::vector<ToolCallDelta> tool_calls;
};

struct Choice {
    std::int32_t index = 0;
    std::optional<Delta> delta;
    std::optional<std::string> finish_reason;
};

struct ApiError {
    std::string message;
    std::optional<std::string> type;
    std::optional<std::string> code;
};

struct Usage {
    std::optional<std::int32_t> prompt_tokens;
    std::optional<std::int32_t> completion_tokens;
    std::optional<std::int32_t> total_tokens;
    std::optional<std::int32_t> prompt_cache_hit_tokens;
    std::optional<std::int32_t> prompt_cache_miss_tokens;
    std::optional<std::int32_t> cache_write_tokens;
    std::optional<std::int32_t> cached_tokens;
    std::optional<std::int32_t> reasoning_tokens;
};

struct Response {
    std::optional<std::string> id;
    std::vector<Choice> choices;
    std::optional<std::string> outcome;
    std::optional<ApiError> error;
    std::optional<Usage> usage;
};

void decode_reasoning_detail(const json::Value& value) {
    require_object(value, "reasoning detail");
    for (const json::Member& entry : value.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "type" || key == "text") {
            (void)decode_optional_string(member, key.c_str());
        }
    }
}

std::vector<ToolCallDelta> decode_tool_calls(const json::Value& value, std::string_view source) {
    if (is_explicit_null(value)) return {};
    if (value.kind != json::Kind::Array) fail("tool_calls must be an array");
    std::vector<ToolCallDelta> calls;
    calls.reserve(value.elements.size());
    for (const json::Value& element : value.elements) {
        require_object(element, "tool call");
        ToolCallDelta call;
        for (const json::Member& entry : element.members) {
            const std::string& key = entry.key;
            const json::Value& member = entry.value;
            if (key == "index") {
                call.index = decode_optional_int(source, member, "index");
            } else if (key == "id") {
                call.id = decode_optional_string(member, "id");
            } else if (key == "type") {
                call.type = decode_optional_string(member, "type");
            } else if (key == "function") {
                if (is_explicit_null(member)) {
                    call.function = std::nullopt;
                    continue;
                }
                require_object(member, "function");
                FunctionDelta function;
                for (const json::Member& function_entry : member.members) {
                    const std::string& function_key = function_entry.key;
                    const json::Value& function_member = function_entry.value;
                    if (function_key == "name") {
                        function.name = decode_optional_string(function_member, "name");
                    } else if (function_key == "arguments") {
                        // explicitNulls = false: an explicit null reads as a
                        // Kotlin null and the accumulator is not fed.
                        if (is_explicit_null(function_member)) continue;
                        function.arguments_present = true;
                        function.arguments = &function_member;
                    }
                }
                call.function = std::move(function);
            }
        }
        calls.push_back(std::move(call));
    }
    return calls;
}

Delta decode_delta(const json::Value& value, std::string_view source) {
    require_object(value, "delta");
    Delta delta;
    for (const json::Member& entry : value.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "role") {
            delta.role = decode_optional_string(member, "role");
        } else if (key == "content") {
            delta.content = decode_optional_string(member, "content");
        } else if (key == "reasoning_content") {
            delta.reasoning_content = decode_optional_string(member, "reasoning_content");
        } else if (key == "reasoning") {
            delta.reasoning = decode_optional_string(member, "reasoning");
        } else if (key == "reasoning_details") {
            if (is_explicit_null(member)) continue;
            if (member.kind != json::Kind::Array) fail("reasoning_details must be an array");
            for (const json::Value& detail : member.elements) decode_reasoning_detail(detail);
        } else if (key == "tool_calls") {
            delta.tool_calls = decode_tool_calls(member, source);
        }
    }
    return delta;
}

Choice decode_choice(const json::Value& value, std::string_view source) {
    require_object(value, "choice");
    Choice choice;
    bool seen_index = false;
    for (const json::Member& entry : value.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "index") {
            choice.index = decode_required_int(source, member, "index");
            seen_index = true;
        } else if (key == "delta") {
            if (is_explicit_null(member)) {
                choice.delta = std::nullopt;
            } else {
                choice.delta = decode_delta(member, source);
            }
        } else if (key == "finish_reason") {
            choice.finish_reason = decode_optional_string(member, "finish_reason");
        }
    }
    if (!seen_index) fail("Choice is missing its index");
    return choice;
}

ApiError decode_api_error(const json::Value& value) {
    require_object(value, "error");
    ApiError error;
    bool seen_message = false;
    for (const json::Member& entry : value.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "message") {
            error.message = decode_required_string(member, "message");
            seen_message = true;
        } else if (key == "type") {
            error.type = decode_optional_string(member, "type");
        } else if (key == "code") {
            error.code = decode_optional_string(member, "code");
        }
    }
    if (!seen_message) fail("Error object is missing its message");
    return error;
}

Usage decode_usage(const json::Value& value, std::string_view source) {
    require_object(value, "usage");
    Usage usage;
    for (const json::Member& entry : value.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "prompt_tokens") {
            usage.prompt_tokens = decode_optional_int(source, member, "prompt_tokens");
        } else if (key == "completion_tokens") {
            usage.completion_tokens = decode_optional_int(source, member, "completion_tokens");
        } else if (key == "total_tokens") {
            usage.total_tokens = decode_optional_int(source, member, "total_tokens");
        } else if (key == "prompt_cache_hit_tokens") {
            usage.prompt_cache_hit_tokens =
                decode_optional_int(source, member, "prompt_cache_hit_tokens");
        } else if (key == "prompt_cache_miss_tokens") {
            usage.prompt_cache_miss_tokens =
                decode_optional_int(source, member, "prompt_cache_miss_tokens");
        } else if (key == "cache_write_tokens") {
            usage.cache_write_tokens = decode_optional_int(source, member, "cache_write_tokens");
        } else if (key == "prompt_tokens_details") {
            if (is_explicit_null(member)) continue;
            require_object(member, "prompt_tokens_details");
            for (const json::Member& detail_entry : member.members) {
                if (detail_entry.key == "cached_tokens") {
                    usage.cached_tokens =
                        decode_optional_int(source, detail_entry.value, "cached_tokens");
                }
            }
        } else if (key == "completion_tokens_details") {
            if (is_explicit_null(member)) continue;
            require_object(member, "completion_tokens_details");
            for (const json::Member& detail_entry : member.members) {
                if (detail_entry.key == "reasoning_tokens") {
                    usage.reasoning_tokens =
                        decode_optional_int(source, detail_entry.value, "reasoning_tokens");
                }
            }
        }
    }
    return usage;
}

Response decode_response(const json::Value& root, std::string_view source) {
    require_object(root, "stream payload");
    Response response;
    for (const json::Member& entry : root.members) {
        const std::string& key = entry.key;
        const json::Value& member = entry.value;
        if (key == "id") {
            response.id = decode_optional_string(member, "id");
        } else if (key == "choices") {
            response.choices.clear();
            if (is_explicit_null(member)) continue;
            if (member.kind != json::Kind::Array) fail("choices must be an array");
            response.choices.reserve(member.elements.size());
            for (const json::Value& element : member.elements) {
                response.choices.push_back(decode_choice(element, source));
            }
        } else if (key == "outcome") {
            response.outcome = decode_optional_string(member, "outcome");
        } else if (key == "error") {
            if (is_explicit_null(member)) {
                response.error = std::nullopt;
            } else {
                response.error = decode_api_error(member);
            }
        } else if (key == "usage") {
            if (is_explicit_null(member)) {
                response.usage = std::nullopt;
            } else {
                response.usage = decode_usage(member, source);
            }
        }
    }
    return response;
}

// ---------------------------------------------------------------------------
// Usage mapping (OpenAiUsage.toTokenUsage) and JsonElement content extraction.
// ---------------------------------------------------------------------------

std::optional<std::int32_t> coerce_at_least_zero(std::optional<std::int32_t> value) {
    if (value && *value < 0) *value = 0;
    return value;
}

std::int32_t add_counts(std::int32_t first, std::int32_t second) {
    const std::int64_t sum = static_cast<std::int64_t>(first) + static_cast<std::int64_t>(second);
    return static_cast<std::int32_t>(sum < INT32_MAX ? sum : INT32_MAX);
}

TokenUsage to_token_usage(const Usage& usage) {
    const std::optional<std::int32_t> input = coerce_at_least_zero(usage.prompt_tokens);
    std::optional<std::int32_t> cached_source = usage.prompt_cache_hit_tokens;
    if (!cached_source) cached_source = usage.cached_tokens;
    const std::optional<std::int32_t> cached = coerce_at_least_zero(cached_source);
    std::optional<std::int32_t> uncached_source = usage.prompt_cache_miss_tokens;
    if (!uncached_source && input && cached) {
        const std::int64_t difference =
            static_cast<std::int64_t>(*input) - static_cast<std::int64_t>(*cached);
        uncached_source = static_cast<std::int32_t>(difference > 0 ? difference : 0);
    }
    const std::optional<std::int32_t> uncached = coerce_at_least_zero(uncached_source);
    const std::optional<std::int32_t> output = coerce_at_least_zero(usage.completion_tokens);
    const std::optional<std::int32_t> reasoning = coerce_at_least_zero(usage.reasoning_tokens);
    // listOfNotNull(input, output).sum(): Int arithmetic; overflow wraps like the JVM.
    std::int32_t derived_total = 0;
    if (input) {
        derived_total = static_cast<std::int32_t>(static_cast<std::uint32_t>(derived_total) +
                                                  static_cast<std::uint32_t>(*input));
    }
    if (output) {
        derived_total = static_cast<std::int32_t>(static_cast<std::uint32_t>(derived_total) +
                                                  static_cast<std::uint32_t>(*output));
    }
    const std::int32_t total_source = usage.total_tokens ? *usage.total_tokens : derived_total;
    TokenUsage result;
    result.total_token_count = total_source < 0 ? 0 : total_source;
    if (input) {
        result.input_token_count = input;
    } else if (cached && uncached) {
        result.input_token_count = add_counts(*cached, *uncached);
    }
    result.cached_input_token_count = cached;
    result.cache_write_input_token_count = coerce_at_least_zero(usage.cache_write_tokens);
    result.uncached_input_token_count = uncached;
    result.output_token_count = output;
    result.reasoning_token_count = reasoning;
    return result;
}

/** `if (it is JsonPrimitive) it.content else it.toString()` on a decoded element. */
std::string json_element_text(const json::Value& value) {
    if (value.kind == json::Kind::String || value.kind == json::Kind::Literal) {
        return value.text;
    }
    return json::to_kotlin_string(value);
}

// ---------------------------------------------------------------------------
// Event helpers.
// ---------------------------------------------------------------------------

bool carries_model_output(const Event& event) {
    switch (event.kind) {
        case Event::Kind::TextChunk:
            return !event.text.empty();
        case Event::Kind::ThoughtChunk:
            return !event.text.empty() || event.signature.has_value();
        case Event::Kind::ToolCallUpdate:
        case Event::Kind::ToolCallRequest:
        case Event::Kind::ToolCallsRequest:
            return true;
        default:
            return false;
    }
}

void append_string_member(json::Value& object, std::string_view key, std::string text) {
    json::Value member;
    member.kind = json::Kind::String;
    member.text = std::move(text);
    object.members.push_back(json::Member{std::string(key), std::move(member)});
}

}  // namespace

// ---------------------------------------------------------------------------
// ToolArgumentAccumulator.
// ---------------------------------------------------------------------------

void ToolArgumentAccumulator::append(std::string_view fragment) {
    if (fragment.empty()) return;
    if (value.empty()) {
        value.assign(fragment);
        return;
    }
    const std::size_t current_length = json::utf16_length(value);
    const std::size_t fragment_length = json::utf16_length(fragment);
    if (current_length >= 2 && fragment_length >= 2) {
        if (fragment == value) return;
        if (fragment_length > current_length &&
            fragment.compare(0, value.size(), value) == 0) {
            value.assign(fragment);
            return;
        }
        if (fragment_length >= 16 && fragment_length * 2 >= current_length &&
            value.compare(0, fragment.size(), fragment) == 0) {
            return;
        }
    }
    value.append(fragment);
}

// ---------------------------------------------------------------------------
// StreamTermination.
// ---------------------------------------------------------------------------

bool Termination::truncated_by_token_cap() const {
    if (!stop_reason) return false;
    return *stop_reason == "max_tokens" || *stop_reason == "length" ||
           *stop_reason == "max_output_tokens";
}

bool Termination::is_retryable() const {
    if (produced_content || already_reported_error || truncated_by_token_cap()) return false;
    if (timed_out) return true;
    if (stream_error.kind != Error::Kind::None &&
        (retryable_stream_error || should_retry_stream_error(stream_error))) {
        return true;
    }
    return tool_call_in_flight || !saw_terminal_marker;
}

Error Termination::to_error(std::string_view provider) const {
    if (already_reported_error) return Error{};
    if (stream_error.kind != Error::Kind::None) return stream_error;
    if (timed_out) {
        Error error;
        error.kind = Error::Kind::Timeout;
        return error;
    }
    if (truncated_by_token_cap()) {
        Error error;
        error.kind = Error::Kind::OutputTruncated;
        error.provider = std::string(provider);
        error.stop_reason = stop_reason;
        return error;
    }
    if (tool_call_in_flight || !saw_terminal_marker) {
        Error error;
        error.kind = Error::Kind::IncompleteStream;
        error.provider = std::string(provider);
        error.stop_reason = stop_reason;
        error.tool_call_in_flight = tool_call_in_flight;
        error.produced_content = produced_content;
        return error;
    }
    return Error{};
}

std::string Termination::describe() const {
    std::string out = "stop_reason=" + (stop_reason ? *stop_reason : std::string("null"));
    out += " terminal_marker=" + std::string(saw_terminal_marker ? "true" : "false");
    out += " produced_content=" + std::string(produced_content ? "true" : "false");
    out += " tool_in_flight=" + std::string(tool_call_in_flight ? "true" : "false");
    out += " timed_out=" + std::string(timed_out ? "true" : "false");
    out += " stream_error=" + std::string(stream_error.kind != Error::Kind::None ? "true" : "false");
    out += " reported_error=" + std::string(already_reported_error ? "true" : "false");
    out += " retryable=" + std::string(is_retryable() ? "true" : "false");
    return out;
}

// ---------------------------------------------------------------------------
// ChatStreamConsumer.
// ---------------------------------------------------------------------------

ChatStreamConsumer::ChatStreamConsumer() : ChatStreamConsumer(Options{}) {}

ChatStreamConsumer::ChatStreamConsumer(Options options) : options_(std::move(options)) {}

std::string ChatStreamConsumer::next_uuid() const {
    if (options_.uuid) return options_.uuid();
    return random_uuid_v4();
}

ChatStreamConsumer::PendingToolCall& ChatStreamConsumer::get_or_create(std::int32_t index) {
    for (auto& entry : pending_) {
        if (entry.first == index) return entry.second;
    }
    PendingToolCall created;
    created.stream_key = "call_stream_" + next_uuid();
    pending_.emplace_back(index, std::move(created));
    return pending_.back().second;
}

void ChatStreamConsumer::emit_tracked(LineOutcome& outcome, Event event) {
    if (carries_model_output(event)) produced_content_ = true;
    if (event.kind == Event::Kind::Error) reported_error_ = true;
    outcome.events.push_back(std::move(event));
}

void ChatStreamConsumer::emit_pending(LineOutcome& outcome) {
    if (pending_.empty()) return;
    std::vector<std::pair<std::int32_t, PendingToolCall>> pending;
    pending.reserve(pending_.size());
    for (auto& entry : pending_) pending.push_back(std::move(entry));
    pending_.clear();

    const auto malformed = [&](const std::string& cause, const std::string& original_name,
                               const std::string& original_arguments,
                               const std::string& source_stream_key) -> ToolCall {
        std::string suffix;
        for (char c : next_uuid()) {
            if (c != '-') suffix.push_back(c);
        }
        json::Value object;
        object.kind = json::Kind::Object;
        append_string_member(object, "error", cause);
        if (!original_name.empty()) {
            append_string_member(object, "original_name",
                                 json::take_utf16_units(original_name, 200));
        }
        if (!original_arguments.empty()) {
            append_string_member(object, "original_arguments",
                                 json::take_utf16_units(original_arguments, 2000));
        }
        ToolCall call;
        call.id = "call_malformed_" + suffix;
        call.name = "agora_malformed_tool_call";
        call.arguments = json::to_kotlin_string(object);
        call.signature = std::nullopt;
        call.stream_key = json::is_blank(source_stream_key) ? ("malformed_tool_" + suffix)
                                                            : source_stream_key;
        return call;
    };

    std::set<std::string> seen_ids;
    std::vector<ToolCall> calls;
    calls.reserve(pending.size());
    for (auto& [index, candidate] : pending) {
        const std::string& args_text = candidate.args.value;
        std::string call_id = json::is_blank(candidate.id) ? candidate.stream_key : candidate.id;
        std::string arguments = json::is_blank(args_text) ? std::string("{}") : args_text;
        bool arguments_are_object = false;
        {
            json::ParseResult parsed = json::parse(arguments);
            arguments_are_object = parsed.value && parsed.value->kind == json::Kind::Object;
        }
        if (!matches_safe_wire_id(call_id)) {
            calls.push_back(malformed("Provider returned an invalid tool call id", candidate.name,
                                      arguments, candidate.stream_key));
        } else if (!seen_ids.insert(call_id).second) {
            calls.push_back(malformed("Provider returned duplicate tool call ids", candidate.name,
                                      arguments, candidate.stream_key));
        } else if (!matches_safe_wire_name(candidate.name)) {
            calls.push_back(malformed("Provider ended before the tool name was complete",
                                      candidate.name, arguments, candidate.stream_key));
        } else if (!arguments_are_object) {
            calls.push_back(malformed(
                "Provider ended before the tool arguments formed a complete JSON object",
                candidate.name, arguments, candidate.stream_key));
        } else {
            ToolCall call;
            call.id = call_id;
            call.name = candidate.name;
            call.arguments = arguments;
            call.stream_key = candidate.stream_key;
            calls.push_back(std::move(call));
        }
    }
    if (calls.size() == 1) {
        Event event;
        event.kind = Event::Kind::ToolCallRequest;
        event.call_id = calls.front().id;
        event.name = calls.front().name;
        event.arguments = calls.front().arguments;
        event.stream_key = calls.front().stream_key;
        emit_tracked(outcome, std::move(event));
    } else {
        Event event;
        event.kind = Event::Kind::ToolCallsRequest;
        event.calls = std::move(calls);
        emit_tracked(outcome, std::move(event));
    }
}

ChatStreamConsumer::LineOutcome ChatStreamConsumer::consume_line(std::string_view line) {
    LineOutcome outcome;
    consecutive_read_timeouts_ = 0;
    if (line.size() < 6 || line.compare(0, 6, "data: ") != 0) return outcome;
    std::string json_str = json::trim(line.substr(6));

    if (json_str == "[DONE]") {
        saw_done_ = true;
        emit_pending(outcome);
        outcome.keep_reading = false;
        return outcome;
    }

    const auto report_sse_parse = [&](const std::string& message) {
        if (options_.log_malformed) {
            options_.log_malformed("[" + options_.provider_name +
                                   "] malformed stream payload exception=" + message);
        }
        Error error;
        error.kind = Error::Kind::SseParse;
        error.raw_line = json::take_utf16_units(json_str, 512);
        error.cause = message;
        stream_error_ = std::move(error);
        outcome.keep_reading = false;
    };

    json::ParseResult parsed = json::parse(json_str);
    if (!parsed.value) {
        report_sse_parse(parsed.error);
        return outcome;
    }
    Response response;
    try {
        response = decode_response(*parsed.value, json_str);
    } catch (const DecodeFailure& failure) {
        report_sse_parse(failure.what());
        return outcome;
    }

    if (response.error) {
        Error error;
        error.kind = Error::Kind::Api;
        error.code = response.error->code;
        error.type = response.error->type;
        error.message = json::is_blank(response.error->message)
                            ? "Provider reported an error in the response stream"
                            : response.error->message;
        stream_error_ = std::move(error);
    }
    if (stream_error_.kind == Error::Kind::None && is_failed_to_generate(response.outcome)) {
        Error error;
        error.kind = Error::Kind::Api;
        error.type = std::string("failed_to_generate");
        error.message = response.outcome.value_or("");
        stream_error_ = std::move(error);
    }

    if (!response.choices.empty()) {
        const Choice& choice = response.choices.front();
        if (choice.delta) {
            const Delta& delta = *choice.delta;
            // defaultOpenAiDeltaContent: reasoning_content first, bare reasoning
            // as the fallback, then the answer text.
            if (delta.reasoning_content && !delta.reasoning_content->empty()) {
                Event event;
                event.kind = Event::Kind::ThoughtChunk;
                event.text = *delta.reasoning_content;
                emit_tracked(outcome, std::move(event));
            } else if (delta.reasoning && !delta.reasoning->empty()) {
                Event event;
                event.kind = Event::Kind::ThoughtChunk;
                event.text = *delta.reasoning;
                emit_tracked(outcome, std::move(event));
            }
            if (delta.content && !delta.content->empty()) {
                Event event;
                event.kind = Event::Kind::TextChunk;
                event.text = *delta.content;
                emit_tracked(outcome, std::move(event));
            }
            for (const ToolCallDelta& tc : delta.tool_calls) {
                const PendingToolCall* existing = nullptr;
                std::int32_t existing_key = 0;
                if (tc.id) {
                    for (const auto& entry : pending_) {
                        if (entry.second.id == *tc.id) {
                            existing = &entry.second;
                            existing_key = entry.first;
                            break;
                        }
                    }
                }
                std::int32_t index;
                if (tc.index) {
                    index = *tc.index;
                } else if (existing) {
                    index = existing_key;
                } else if (pending_.size() == 1) {
                    index = pending_.front().first;
                } else {
                    index = static_cast<std::int32_t>(pending_.size());
                }
                PendingToolCall& pending = get_or_create(index);
                if (tc.id && !json::is_blank(*tc.id)) pending.id = *tc.id;
                if (tc.function && tc.function->name && !json::is_blank(*tc.function->name)) {
                    pending.name = *tc.function->name;
                }
                if (tc.function && tc.function->arguments_present && tc.function->arguments) {
                    pending.args.append(json_element_text(*tc.function->arguments));
                }
                Event event;
                event.kind = Event::Kind::ToolCallUpdate;
                event.stream_key = pending.stream_key;
                if (!json::is_blank(pending.id)) event.call_id = pending.id;
                event.name = pending.name;
                event.arguments = pending.args.to_string();
                emit_tracked(outcome, std::move(event));
            }
        }
        if (choice.finish_reason && !json::is_blank(*choice.finish_reason)) {
            finish_reason_ = lowercase_ascii(*choice.finish_reason);
            emit_pending(outcome);
        }
    }

    if (response.usage) {
        Event event;
        event.kind = Event::Kind::UsageUpdate;
        event.usage = to_token_usage(*response.usage);
        emit_tracked(outcome, std::move(event));
    }

    if (stream_error_.kind != Error::Kind::None) outcome.keep_reading = false;
    return outcome;
}

bool ChatStreamConsumer::on_read_timeout(bool terminal_grace_active) {
    if (terminal_grace_active) return false;
    consecutive_read_timeouts_++;
    if (consecutive_read_timeouts_ >= 3) {
        timed_out_ = true;
        return false;
    }
    return true;
}

void ChatStreamConsumer::on_read_error(Error error) { stream_error_ = std::move(error); }

Termination ChatStreamConsumer::finish() const {
    Termination termination;
    termination.saw_terminal_marker = saw_done_ || finish_reason_.has_value();
    termination.stop_reason = finish_reason_;
    termination.produced_content = produced_content_;
    termination.tool_call_in_flight = !pending_.empty();
    termination.stream_error = stream_error_;
    termination.already_reported_error = reported_error_;
    termination.timed_out = timed_out_;
    return termination;
}

}  // namespace agora::core::openai
