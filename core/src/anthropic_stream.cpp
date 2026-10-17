#include "agora/core/anthropic_stream.hpp"

#include <algorithm>
#include <cmath>
#include <cstddef>
#include <cstdint>
#include <cstring>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <variant>
#include <vector>

#include "jvm_pow10_table.generated.hpp"

// Port of AnthropicStreamEventRouter. The private @Serializable event models and the kotlinx
// typed-decode walk are reproduced here (the provider passes its own Json configuration:
// ignoreUnknownKeys = true; encodeDefaults = true; explicitNulls = false). The numeric-literal
// reader is the same consumeNumericLiteral port the openai and citations modules carry: quoted
// integers are accepted, floats and escaped tokens are not.

namespace agora::core::anthropic_stream {

namespace sn = agora::core::stream_normalization;

namespace {

// ---------------------------------------------------------------------------------------------
// consumeNumericLiteral: the exact kotlinx number decode (Long wrap included).

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

/** 10.0.pow(exponent) with the JVM bit patterns, like the sibling modules. */
double pow10_like_jvm(double exponent) {
    if (exponent >= -400.0 && exponent <= 400.0 && std::floor(exponent) == exponent) {
        const auto index = static_cast<int>(exponent) + 400;
        double result = 0.0;
        std::memcpy(&result, &openai::kJvmPow10Bits[index], sizeof(result));
        return result;
    }
    return std::pow(10.0, exponent);
}

double calculate_exponent(std::int64_t exponent_accumulator, bool is_exponent_positive) {
    const double value = static_cast<double>(exponent_accumulator);
    return is_exponent_positive ? pow10_like_jvm(value) : pow10_like_jvm(-value);
}

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

/** An Int property fed any token through consumeNumericLiteral; quoted numbers pass, floats and
 *  every non-numeric token fail. Explicit null is handled by the caller (nullable fields). */
std::int32_t decode_int_token(std::string_view source, const json::Value& value) {
    std::int64_t parsed = 0;
    if (!decode_numeric_literal(source, value.begin, parsed)) {
        throw DecodeFailure{false, "Expected int literal"};
    }
    if (parsed != static_cast<std::int64_t>(static_cast<std::int32_t>(parsed))) {
        throw DecodeFailure{false, "Int overflow"};
    }
    return static_cast<std::int32_t>(parsed);
}

// ---------------------------------------------------------------------------------------------
// Small helpers.

bool is_json_null(const json::Value& value) {
    return value.kind == json::Kind::Literal && value.text == "null";
}

const json::Value& require_object(const json::Value& value, const char* what) {
    if (value.kind != json::Kind::Object) {
        throw DecodeFailure{false, std::string(what) + " must be a JSON object"};
    }
    return value;
}

/** Kotlin String.lowercase() restricted to ASCII (see the header's boundary note). */
std::string ascii_lower(std::string_view text) {
    std::string out(text);
    for (char& c : out) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return out;
}

/** safeWireToolName / safeWireToolCallId: single-character-class full matches. */
bool matches_safe_wire(std::string_view value, std::size_t max_units) {
    std::size_t units = 0;
    for (char c : value) {
        const bool allowed = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
            (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!allowed) return false;
        ++units;
    }
    return units >= 1 && units <= max_units;
}

/** Kotlin Int addition for answerLength (wrap on overflow, like the JVM). */
std::int32_t wrap_add(std::int32_t first, std::int32_t second) {
    return static_cast<std::int32_t>(static_cast<std::uint32_t>(first) +
                                     static_cast<std::uint32_t>(second));
}

/** model.TokenUsage.addCounts: sum, capped at Int.MAX_VALUE, truncated like Int.toInt(). */
std::int32_t add_counts(std::int32_t first, std::int32_t second) {
    const std::int64_t sum =
        static_cast<std::int64_t>(first) + static_cast<std::int64_t>(second);
    const std::int64_t capped = sum > 2147483647LL ? 2147483647LL : sum;
    return static_cast<std::int32_t>(capped);
}

std::int32_t coerce_at_least_zero(std::int32_t value) {
    return value < 0 ? 0 : value;
}

// ---------------------------------------------------------------------------------------------
// The typed walk over the parsed document (decodeFromString<AnthropicStreamEvent>).

std::optional<std::string> decode_string_nullable(
    const json::Value& value,
    const char* what) {
    if (is_json_null(value)) return std::nullopt;
    if (value.kind != json::Kind::String) {
        throw DecodeFailure{false, std::string(what) + " must be a JSON string"};
    }
    return value.text;
}

std::string decode_string_required(const json::Value& value, const char* what) {
    if (value.kind != json::Kind::String) {
        throw DecodeFailure{false, std::string(what) + " must be a JSON string"};
    }
    return value.text;
}

std::optional<std::int32_t> decode_int_nullable(std::string_view source, const json::Value& value) {
    if (is_json_null(value)) return std::nullopt;
    return decode_int_token(source, value);
}

Usage decode_usage(std::string_view source, const json::Value& value) {
    require_object(value, "usage");
    Usage usage;
    for (const json::Member& member : value.members) {
        if (member.key == "input_tokens") {
            usage.input_tokens = decode_int_nullable(source, member.value);
        } else if (member.key == "output_tokens") {
            usage.output_tokens = decode_int_nullable(source, member.value);
        } else if (member.key == "cache_creation_input_tokens") {
            usage.cache_creation_input_tokens = decode_int_nullable(source, member.value);
        } else if (member.key == "cache_read_input_tokens") {
            usage.cache_read_input_tokens = decode_int_nullable(source, member.value);
        }
    }
    return usage;
}

StreamErrorPayload decode_stream_error(const json::Value& value) {
    require_object(value, "error");
    StreamErrorPayload error;
    for (const json::Member& member : value.members) {
        if (member.key == "type") {
            error.type = decode_string_nullable(member.value, "error.type");
        } else if (member.key == "message") {
            error.message = decode_string_nullable(member.value, "error.message");
        }
    }
    return error;
}

Delta decode_delta(const json::Value& value) {
    require_object(value, "delta");
    Delta delta;
    for (const json::Member& member : value.members) {
        if (member.key == "text") {
            delta.text = decode_string_nullable(member.value, "delta.text");
        } else if (member.key == "thinking") {
            delta.thinking = decode_string_nullable(member.value, "delta.thinking");
        } else if (member.key == "signature") {
            delta.signature = decode_string_nullable(member.value, "delta.signature");
        } else if (member.key == "partial_json") {
            delta.partial_json = decode_string_nullable(member.value, "delta.partial_json");
        } else if (member.key == "citation") {
            if (!is_json_null(member.value)) delta.citation = member.value;
        } else if (member.key == "type") {
            delta.type = decode_string_nullable(member.value, "delta.type");
        } else if (member.key == "stop_reason") {
            delta.stop_reason = decode_string_nullable(member.value, "delta.stop_reason");
        } else if (member.key == "stop_sequence") {
            delta.stop_sequence = decode_string_nullable(member.value, "delta.stop_sequence");
        }
    }
    return delta;
}

ContentBlock decode_content_block(const json::Value& value) {
    require_object(value, "content_block");
    ContentBlock block;
    bool has_type = false;
    for (const json::Member& member : value.members) {
        if (member.key == "type") {
            block.type = decode_string_required(member.value, "content_block.type");
            has_type = true;
        } else if (member.key == "id") {
            block.id = decode_string_nullable(member.value, "content_block.id");
        } else if (member.key == "name") {
            block.name = decode_string_nullable(member.value, "content_block.name");
        } else if (member.key == "input") {
            if (!is_json_null(member.value)) {
                require_object(member.value, "content_block.input");
                block.input = member.value;
            }
        } else if (member.key == "text") {
            block.text = decode_string_nullable(member.value, "content_block.text");
        } else if (member.key == "citations") {
            if (!is_json_null(member.value)) {
                if (member.value.kind != json::Kind::Array) {
                    throw DecodeFailure{false, "content_block.citations must be a JSON array"};
                }
                std::vector<json::Value> citations;
                citations.reserve(member.value.elements.size());
                for (const json::Value& element : member.value.elements) {
                    citations.push_back(element);
                }
                block.citations = std::move(citations);
            }
        } else if (member.key == "thinking") {
            block.thinking = decode_string_nullable(member.value, "content_block.thinking");
        } else if (member.key == "signature") {
            block.signature = decode_string_nullable(member.value, "content_block.signature");
        }
    }
    if (!has_type) throw DecodeFailure{true, "Missing field 'type'"};
    return block;
}

MessageInfo decode_message_info(std::string_view source, const json::Value& value) {
    require_object(value, "message");
    MessageInfo message;
    for (const json::Member& member : value.members) {
        if (member.key == "usage") {
            if (!is_json_null(member.value)) message.usage = decode_usage(source, member.value);
        } else if (member.key == "stop_reason") {
            message.stop_reason = decode_string_nullable(member.value, "message.stop_reason");
        }
    }
    return message;
}

sn::ErrorApi to_generation_error(const std::optional<StreamErrorPayload>& error) {
    sn::ErrorApi api;
    api.code = std::nullopt;
    api.type = error ? error->type : std::nullopt;
    if (error && error->message && !json::is_blank(*error->message)) {
        api.message = *error->message;
    } else {
        api.message = "Provider reported an error in the response stream";
    }
    return api;
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// json_element_equal.

bool json_element_equal(const json::Value& first, const json::Value& second) {
    const auto is_primitive = [](const json::Value& value) {
        return value.kind == json::Kind::String || value.kind == json::Kind::Literal;
    };
    if (is_primitive(first) || is_primitive(second)) {
        if (!(is_primitive(first) && is_primitive(second))) return false;
        const bool first_is_string = first.kind == json::Kind::String;
        const bool second_is_string = second.kind == json::Kind::String;
        return first_is_string == second_is_string && first.text == second.text;
    }
    if (first.kind != second.kind) return false;
    if (first.kind == json::Kind::Array) {
        if (first.elements.size() != second.elements.size()) return false;
        for (std::size_t i = 0; i < first.elements.size(); ++i) {
            if (!json_element_equal(first.elements[i], second.elements[i])) return false;
        }
        return true;
    }
    // JsonObject equality is Map equality: duplicate keys were collapsed by the lexer
    // (last value wins), so collapse before comparing.
    std::map<std::string, const json::Value*> left;
    for (const json::Member& member : first.members) left[member.key] = &member.value;
    std::map<std::string, const json::Value*> right;
    for (const json::Member& member : second.members) right[member.key] = &member.value;
    if (left.size() != right.size()) return false;
    for (const auto& [key, value] : left) {
        const auto other = right.find(key);
        if (other == right.end() || !json_element_equal(*value, *other->second)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// carries_model_output.

bool carries_model_output(const StreamEvent& event) {
    struct Visitor {
        bool operator()(const TextChunk& chunk) const { return !chunk.text.empty(); }
        bool operator()(const CitationUpdate&) const { return false; }
        bool operator()(const ThoughtChunk& chunk) const {
            return !chunk.thought.empty() || chunk.signature.has_value();
        }
        bool operator()(const UsageUpdate&) const { return false; }
        bool operator()(const ToolCallUpdate&) const { return true; }
        bool operator()(const ToolCallRequest&) const { return true; }
        bool operator()(const ErrorEvent&) const { return false; }
    };
    return std::visit(Visitor{}, event);
}

// ---------------------------------------------------------------------------------------------
// decode_stream_event.

std::optional<AnthropicStreamEvent> decode_stream_event(
    std::string_view source,
    DecodeFailure* failure) {
    try {
        const json::ParseResult parsed = json::parse(source);
        if (!parsed.value) {
            throw DecodeFailure{false, "Malformed JSON payload"};
        }
        const json::Value& root = *parsed.value;
        if (root.kind != json::Kind::Object) {
            throw DecodeFailure{false, "Expected the root to be a JSON object"};
        }
        AnthropicStreamEvent event;
        bool has_type = false;
        for (const json::Member& member : root.members) {
            if (member.key == "type") {
                event.type = decode_string_required(member.value, "type");
                has_type = true;
            } else if (member.key == "delta") {
                if (!is_json_null(member.value)) event.delta = decode_delta(member.value);
            } else if (member.key == "content_block") {
                if (!is_json_null(member.value)) {
                    event.content_block = decode_content_block(member.value);
                }
            } else if (member.key == "message") {
                if (!is_json_null(member.value)) {
                    event.message = decode_message_info(source, member.value);
                }
            } else if (member.key == "usage") {
                if (!is_json_null(member.value)) event.usage = decode_usage(source, member.value);
            } else if (member.key == "index") {
                event.index = decode_int_nullable(source, member.value);
            } else if (member.key == "outcome") {
                event.outcome = decode_string_nullable(member.value, "outcome");
            } else if (member.key == "error") {
                if (!is_json_null(member.value)) {
                    event.error = decode_stream_error(member.value);
                }
            }
        }
        if (!has_type) throw DecodeFailure{true, "Missing field 'type'"};
        return event;
    } catch (const DecodeFailure& decode_failure) {
        if (failure != nullptr) *failure = decode_failure;
        return std::nullopt;
    }
}

// ---------------------------------------------------------------------------------------------
// AnthropicStreamEventRouter.

AnthropicStreamEventRouter::AnthropicStreamEventRouter(UuidFn uuid) : uuid_(std::move(uuid)) {}

bool AnthropicStreamEventRouter::saw_terminal_marker() const {
    return message_stop_received || stop_reason.has_value();
}

bool AnthropicStreamEventRouter::tool_call_in_flight() const {
    return !tool_blocks_.empty();
}

std::string AnthropicStreamEventRouter::next_uuid() const {
    return uuid_ ? uuid_() : sn::random_uuid_v4();
}

std::int32_t AnthropicStreamEventRouter::index_for_start(const std::optional<std::int32_t>& index) {
    const std::int32_t resolved = index ? *index : synthetic_index_++;
    last_block_index_ = resolved;
    return resolved;
}

std::int32_t AnthropicStreamEventRouter::index_for_continuation(
    const std::optional<std::int32_t>& index) {
    if (index) return *index;
    if (last_block_index_ >= 0) return last_block_index_;
    return synthetic_index_++;
}

ToolCallUpdate AnthropicStreamEventRouter::update_event(const ToolBlock& tool) const {
    ToolCallUpdate update;
    update.stream_key = tool.stream_key;
    update.id = tool.id;
    update.name = tool.name;
    update.arguments = tool.arguments.str();
    update.signature = std::nullopt;
    return update;
}

ToolCallRequest AnthropicStreamEventRouter::complete_event(const ToolBlock& tool) const {
    ToolCallRequest call;
    call.id = tool.id ? *tool.id : tool.stream_key;
    call.name = tool.name;
    call.arguments = tool.arguments.str();
    if (json::is_blank(call.arguments)) call.arguments = "{}";
    call.signature = std::nullopt;
    call.stream_key = tool.stream_key;
    return call;
}

std::optional<std::string> AnthropicStreamEventRouter::invalid_completion_cause(
    const ToolBlock& tool) const {
    const std::string effective_id = tool.id ? *tool.id : tool.stream_key;
    if (!matches_safe_wire(effective_id, 128)) {
        return "Provider returned an invalid tool call id";
    }
    if (completed_tool_call_ids_.count(effective_id) != 0) {
        return "Provider returned a duplicate tool call id";
    }
    if (!matches_safe_wire(tool.name, 64)) {
        return "Provider ended before the tool name was complete";
    }
    std::string raw_arguments = tool.arguments.str();
    if (json::is_blank(raw_arguments)) raw_arguments = "{}";
    const json::ParseResult parsed = json::parse(raw_arguments);
    if (parsed.value && parsed.value->kind == json::Kind::Object) return std::nullopt;
    return "Provider ended before the tool arguments formed a complete JSON object";
}

void AnthropicStreamEventRouter::emit_citation(
    const TextBlock& block,
    const json::Value& citation,
    std::vector<StreamEvent>& output) {
    std::optional<citations::CitationRecord> record =
        citations::to_anthropic_citation_record(citation, block.start_index, block.text);
    if (record) output.push_back(CitationUpdate{std::move(*record)});
}

void AnthropicStreamEventRouter::append_citation(
    std::int32_t index,
    const json::Value& citation,
    std::vector<StreamEvent>& output) {
    const auto it = text_blocks_.find(index);
    if (it == text_blocks_.end()) return;
    TextBlock& block = it->second;
    for (const json::Value& existing : block.citations) {
        if (json_element_equal(existing, citation)) return;
    }
    if (block.citations.size() >= citations::policy::kMaxSources) return;
    block.citations.push_back(citation);
    emit_citation(block, citation, output);
}

void AnthropicStreamEventRouter::append_text(
    std::int32_t index,
    const std::string& text,
    std::vector<StreamEvent>& output) {
    const auto it = text_blocks_.find(index);
    if (it == text_blocks_.end()) return;
    TextBlock& block = it->second;
    block.text += text;
    answer_length_ = wrap_add(answer_length_, static_cast<std::int32_t>(json::utf16_length(text)));
    output.push_back(TextChunk{text});
    for (const json::Value& citation : block.citations) {
        emit_citation(block, citation, output);
    }
}

void AnthropicStreamEventRouter::route_untyped_delta(
    std::int32_t index,
    const Delta& delta,
    std::vector<StreamEvent>& output) {
    const auto type = block_types_.find(index);
    if (type == block_types_.end()) return;
    if (type->second == "tool_use") {
        const auto tool = tool_blocks_.find(index);
        if (tool == tool_blocks_.end()) return;
        tool->second.arguments.append(delta.partial_json);
        output.push_back(update_event(tool->second));
    } else if (type->second == "thinking") {
        if (delta.signature && !json::is_blank(*delta.signature)) {
            thinking_signatures_[index] = *delta.signature;
        }
        const std::string* content = nullptr;
        if (delta.thinking) {
            content = &*delta.thinking;
        } else if (delta.text) {
            content = &*delta.text;
        }
        if (content != nullptr && !content->empty()) {
            std::optional<std::string> signature;
            const auto stored = thinking_signatures_.find(index);
            if (stored != thinking_signatures_.end()) signature = stored->second;
            output.push_back(ThoughtChunk{*content, std::nullopt, signature});
        }
    } else if (type->second == "text") {
        if (delta.citation) append_citation(index, *delta.citation, output);
        if (delta.text && !delta.text->empty()) append_text(index, *delta.text, output);
    }
}

std::vector<StreamEvent> AnthropicStreamEventRouter::route(const AnthropicStreamEvent& event) {
    std::vector<StreamEvent> output;
    if (event.type == "message_start") {
        if (event.message && event.message->usage) {
            const Usage& usage = *event.message->usage;
            if (usage.input_tokens) input_tokens_ = coerce_at_least_zero(*usage.input_tokens);
            if (usage.cache_creation_input_tokens) {
                cache_creation_input_tokens_ = coerce_at_least_zero(*usage.cache_creation_input_tokens);
            }
            if (usage.cache_read_input_tokens) {
                cache_read_input_tokens_ = coerce_at_least_zero(*usage.cache_read_input_tokens);
            }
        }
    } else if (event.type == "content_block_start") {
        // Kotlin `val block = event.contentBlock ?: return@buildList`: the trailing streamError
        // capture is skipped when the payload has no block at all.
        if (!event.content_block) return output;
        const ContentBlock& block = *event.content_block;
        const std::int32_t index = index_for_start(event.index);
        block_types_[index] = block.type;
        if (block.type == "text") {
            TextBlock text_block;
            text_block.start_index = answer_length_;
            text_blocks_[index] = std::move(text_block);
            if (block.text && !block.text->empty()) append_text(index, *block.text, output);
            if (block.citations) {
                for (const json::Value& citation : *block.citations) {
                    append_citation(index, citation, output);
                }
            }
        } else if (block.type == "thinking") {
            std::optional<std::string> signature;
            if (block.signature && !json::is_blank(*block.signature)) {
                thinking_signatures_[index] = *block.signature;
                signature = *block.signature;
            }
            if (block.thinking && !block.thinking->empty()) {
                output.push_back(ThoughtChunk{*block.thinking, std::nullopt, signature});
            }
        } else if (block.type == "tool_use") {
            ++tool_use_block_starts;
            std::string initial_arguments;
            if (block.input && !(block.input->kind == json::Kind::Object &&
                                 block.input->members.empty())) {
                initial_arguments = json::to_kotlin_string(*block.input);
            }
            std::optional<std::string> id;
            if (block.id && !json::is_blank(*block.id)) id = *block.id;
            ToolBlock tool;
            tool.stream_key = id ? *id : "call_stream_" + next_uuid();
            tool.id = id;
            tool.name = (block.name && !json::is_blank(*block.name)) ? *block.name : "";
            tool.arguments = sn::ToolArgumentAccumulator(initial_arguments);
            tool_blocks_[index] = std::move(tool);
            output.push_back(update_event(tool_blocks_[index]));
        }
    } else if (event.type == "content_block_delta") {
        const std::int32_t index = index_for_continuation(event.index);
        if (!event.delta) return output;
        const Delta& delta = *event.delta;
        if (delta.type == "input_json_delta") {
            const auto tool = tool_blocks_.find(index);
            if (tool == tool_blocks_.end()) return output;
            tool->second.arguments.append(delta.partial_json);
            output.push_back(update_event(tool->second));
        } else if (delta.type == "signature_delta") {
            if (!delta.signature || json::is_blank(*delta.signature)) return output;
            thinking_signatures_[index] = *delta.signature;
            output.push_back(ThoughtChunk{"", std::nullopt, *delta.signature});
        } else if (delta.type == "thinking_delta") {
            if (delta.thinking && !delta.thinking->empty()) {
                std::optional<std::string> signature;
                const auto stored = thinking_signatures_.find(index);
                if (stored != thinking_signatures_.end()) signature = stored->second;
                output.push_back(ThoughtChunk{*delta.thinking, std::nullopt, signature});
            }
        } else if (delta.type == "citations_delta") {
            const auto type = block_types_.find(index);
            if (type != block_types_.end() && type->second == "text" && delta.citation) {
                append_citation(index, *delta.citation, output);
            }
        } else if (delta.type == "text_delta") {
            const auto type = block_types_.find(index);
            if (type != block_types_.end() && type->second == "text" && delta.text &&
                !delta.text->empty()) {
                append_text(index, *delta.text, output);
            }
        } else {
            route_untyped_delta(index, delta, output);
        }
    } else if (event.type == "content_block_stop") {
        const std::int32_t index = index_for_continuation(event.index);
        const auto text_block = text_blocks_.find(index);
        if (text_block != text_blocks_.end()) {
            TextBlock block = std::move(text_block->second);
            text_blocks_.erase(text_block);
            for (const json::Value& citation : block.citations) {
                emit_citation(block, citation, output);
            }
        }
        const auto tool_block = tool_blocks_.find(index);
        if (tool_block != tool_blocks_.end()) {
            ToolBlock tool = std::move(tool_block->second);
            tool_blocks_.erase(tool_block);
            const std::optional<std::string> invalid = invalid_completion_cause(tool);
            if (!invalid) {
                completed_tool_call_ids_.insert(tool.id ? *tool.id : tool.stream_key);
                output.push_back(complete_event(tool));
            } else {
                const sn::ToolCallRequest stand_in = sn::malformed_tool_call_request(
                    *invalid,
                    tool.name,
                    tool.arguments.str(),
                    tool.stream_key,
                    std::nullopt,
                    uuid_);
                ToolCallRequest call;
                call.id = stand_in.id;
                call.name = stand_in.name;
                call.arguments = stand_in.arguments;
                call.signature = stand_in.signature;
                call.stream_key = stand_in.stream_key;
                output.push_back(call);
            }
        }
        block_types_.erase(index);
        thinking_signatures_.erase(index);
    } else if (event.type == "message_delta") {
        const std::optional<std::string> reason =
            (event.delta && event.delta->stop_reason) ? event.delta->stop_reason
                                                      : (event.message ? event.message->stop_reason
                                                                       : std::nullopt);
        if (reason && !json::is_blank(*reason)) stop_reason = ascii_lower(*reason);
        if (event.usage) {
            const Usage& usage = *event.usage;
            if (usage.input_tokens) input_tokens_ = coerce_at_least_zero(*usage.input_tokens);
            if (usage.cache_creation_input_tokens) {
                cache_creation_input_tokens_ = coerce_at_least_zero(*usage.cache_creation_input_tokens);
            }
            if (usage.cache_read_input_tokens) {
                cache_read_input_tokens_ = coerce_at_least_zero(*usage.cache_read_input_tokens);
            }
            std::optional<std::int32_t> uncached_input;
            if (input_tokens_) {
                uncached_input = add_counts(*input_tokens_, cache_creation_input_tokens_);
            }
            std::optional<std::int32_t> total_input;
            if (uncached_input) {
                total_input = add_counts(*uncached_input, cache_read_input_tokens_);
            }
            std::optional<std::int32_t> output_tokens;
            if (usage.output_tokens) output_tokens = coerce_at_least_zero(*usage.output_tokens);
            std::int32_t total = 0;
            if (total_input && output_tokens) {
                total = add_counts(*total_input, *output_tokens);
            } else if (total_input) {
                total = *total_input;
            } else if (output_tokens) {
                total = *output_tokens;
            }
            TokenUsage token_usage;
            token_usage.total_token_count = total;
            token_usage.input_token_count = total_input;
            token_usage.cached_input_token_count =
                total_input ? std::optional<std::int32_t>(cache_read_input_tokens_) : std::nullopt;
            token_usage.cache_write_input_token_count =
                total_input ? std::optional<std::int32_t>(cache_creation_input_tokens_)
                            : std::nullopt;
            token_usage.uncached_input_token_count = uncached_input;
            token_usage.output_token_count = output_tokens;
            output.push_back(UsageUpdate{token_usage});
        }
    } else if (event.type == "message_stop") {
        message_stop_received = true;
        const std::optional<std::string> reason =
            (event.delta && event.delta->stop_reason) ? event.delta->stop_reason
                                                      : (event.message ? event.message->stop_reason
                                                                       : std::nullopt);
        if (reason && !json::is_blank(*reason)) stop_reason = ascii_lower(*reason);
    } else if (event.type == "error") {
        stream_error = to_generation_error(event.error);
    }

    // A relay may send the error payload without the `error` event type, or attach it to an
    // otherwise-unknown event. The early returns above skip this, exactly like return@buildList.
    if (!stream_error && event.error && event.type != "error") {
        stream_error = to_generation_error(event.error);
    }
    if (!stream_error && sn::ProviderRetryPolicy::is_failed_to_generate_outcome(event.outcome)) {
        stream_error = sn::ErrorApi{std::nullopt,
                                    std::string("failed_to_generate"),
                                    event.outcome ? *event.outcome : std::string()};
    }
    return output;
}

std::vector<StreamEvent> AnthropicStreamEventRouter::report_incomplete_blocks() {
    std::vector<StreamEvent> output;
    for (const auto& [index, tool] : tool_blocks_) {
        (void)index;
        if (!matches_safe_wire(tool.name, 64)) {
            reported_error = true;
            output.push_back(ErrorEvent{sn::ErrorSseParse{
                std::string("stream_end"),
                std::string("Provider ended before the tool name was complete")}});
            break;
        }
    }
    return output;
}

void AnthropicStreamEventRouter::capture_parse_error(
    const std::string& raw_line,
    const std::string& cause) {
    if (stream_error) return;
    stream_error = sn::ErrorSseParse{json::take_utf16_units(raw_line, 512), cause};
}

}  // namespace agora::core::anthropic_stream
