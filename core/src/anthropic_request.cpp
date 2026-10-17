#include "agora/core/anthropic_request.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::anthropic {
namespace {

namespace json = agora::core::json;
namespace openai = agora::core::openai;
namespace prep = agora::core::prep;

// ---------------------------------------------------------------------------------------------
// Small string helpers (Kotlin String semantics as pinned by the shared modules)
// ---------------------------------------------------------------------------------------------

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/** String.equals(ignoreCase = true) for the ASCII wire-identity grammar (ASCII-only folding). */
bool equals_ignore_ascii(std::string_view left, std::string_view right) {
    if (left.size() != right.size()) return false;
    for (std::size_t i = 0; i < left.size(); ++i) {
        char a = left[i];
        char b = right[i];
        if (a >= 'A' && a <= 'Z') a = static_cast<char>(a - 'A' + 'a');
        if (b >= 'A' && b <= 'Z') b = static_cast<char>(b - 'A' + 'a');
        if (a != b) return false;
    }
    return true;
}

/** String.removePrefix("models/"): only an exact prefix is removed. */
std::string_view strip_models_prefix(std::string_view value) {
    constexpr std::string_view kPrefix = "models/";
    if (starts_with(value, kPrefix)) value.remove_prefix(kPrefix.size());
    return value;
}

/** Regex("[A-Za-z0-9_-]{1,N}") matches() on UTF-16 units: ASCII grammar, byte counts equal. */
bool wire_safe(std::string_view value, std::size_t limit) {
    if (value.empty() || value.size() > limit) return false;
    for (char c : value) {
        const bool ok = (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                        (c >= '0' && c <= '9') || c == '_' || c == '-';
        if (!ok) return false;
    }
    return true;
}

bool contains_string(const std::vector<std::string>& values, const std::string& value) {
    for (const std::string& entry : values) {
        if (entry == value) return true;
    }
    return false;
}

bool add_unique(std::vector<std::string>& values, const std::string& value) {
    if (contains_string(values, value)) return false;
    values.push_back(value);
    return true;
}

bool same_elements(const std::vector<std::string>& left, const std::vector<std::string>& right) {
    if (left.size() != right.size()) return false;
    for (const std::string& entry : left) {
        if (!contains_string(right, entry)) return false;
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// ModelId.parse — verbatim semantics (case-sensitive legacy heuristics).
// ---------------------------------------------------------------------------------------------

struct ParsedModelId {
    std::string provider_name;
    std::string model_name;
};

ParsedModelId parse_model_id(const std::string& prefixed) {
    const std::size_t colon = prefixed.find(':');
    if (colon != std::string::npos) {
        return ParsedModelId{prefixed.substr(0, colon), prefixed.substr(colon + 1)};
    }
    std::string provider = "Unknown";
    if (starts_with(prefixed, "gpt-") || starts_with(prefixed, "o1") ||
        starts_with(prefixed, "o3") || starts_with(prefixed, "o4")) {
        provider = "OpenAI";
    } else if (starts_with(prefixed, "claude-")) {
        provider = "Anthropic";
    } else if (prefixed.find("deepseek") != std::string::npos) {
        provider = "DeepSeek";
    } else if (prefixed.find("qwen") != std::string::npos) {
        provider = "Qwen";
    } else if (prefixed.find("groq") != std::string::npos) {
        provider = "Groq";
    } else if (prefixed.find("models/") != std::string::npos || starts_with(prefixed, "gemini")) {
        provider = "Google";
    }
    return ParsedModelId{provider, prefixed};
}

// ---------------------------------------------------------------------------------------------
// Failure shapes
// ---------------------------------------------------------------------------------------------

[[noreturn]] void throw_request_format(const std::string& provider,
                                       const std::vector<std::string>& violations) {
    std::vector<std::string> distinct;
    for (const std::string& violation : violations) {
        add_unique(distinct, violation);
    }
    std::string joined;
    for (std::size_t i = 0; i < distinct.size(); ++i) {
        if (i > 0) joined += "; ";
        joined += distinct[i];
    }
    throw ProviderRequestError{
        "RequestFormatException",
        provider + " request validation failed: " + joined,
    };
}

[[noreturn]] void throw_decoding(const std::string& what) {
    throw ProviderRequestError{"JsonDecodingException", what};
}

[[noreturn]] void throw_missing_field(const std::string& field) {
    throw ProviderRequestError{"MissingFieldException", "Field '" + field + "' is required"};
}

// ---------------------------------------------------------------------------------------------
// JsonObject lookup for the decode half: last duplicate key wins (LinkedHashMap).
// ---------------------------------------------------------------------------------------------

const json::Value* member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& entry : object.members) {
        if (entry.key == name) found = &entry.value;
    }
    return found;
}

bool is_null(const json::Value* value) {
    return value != nullptr && value->kind == json::Kind::Literal && value->text == "null";
}

/** kotlinx String decode: only a JSON string primitive reaches content (numbers, booleans and
 *  null are decode errors — pinned by the golden traces). */
std::string decode_string(const json::Value& value) {
    if (value.kind != json::Kind::String) throw_decoding("expected a JSON string");
    return value.text;
}

std::optional<std::string> decode_optional_string(const json::Value& object,
                                                  std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_string(*value);
}

/**
 * The strict number grammar kotlinx applies to numeric content: -?digits(.digits)?(e[+-]?digits)?
 * (leading zeros are accepted — pinned by the goldens).
 */
bool is_json_number_lexeme(std::string_view text) {
    std::size_t index = 0;
    if (index < text.size() && text[index] == '-') ++index;
    std::size_t digits = 0;
    while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
        ++index;
        ++digits;
    }
    if (digits == 0) return false;
    if (index < text.size() && text[index] == '.') {
        ++index;
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) return false;
    }
    if (index < text.size() && (text[index] == 'e' || text[index] == 'E')) {
        ++index;
        if (index < text.size() && (text[index] == '+' || text[index] == '-')) ++index;
        digits = 0;
        while (index < text.size() && text[index] >= '0' && text[index] <= '9') {
            ++index;
            ++digits;
        }
        if (digits == 0) return false;
    }
    return index == text.size();
}

/**
 * Int decode: the JSON integer lexeme (quoted or not) — no '+', no leading zeros, Int range.
 * Failures are decode errors (pinned by the golden traces).
 */
std::int32_t decode_int_value(const json::Value& value) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        throw_decoding("expected an integer");
    }
    const std::string_view text = value.text;
    if (!is_json_number_lexeme(text)) throw_decoding("expected an Int");
    std::size_t index = 0;
    bool negative = false;
    if (text[index] == '-') {
        negative = true;
        ++index;
    }
    std::uint64_t accumulator = 0;
    const std::uint64_t limit = negative ? 2147483648ull : 2147483647ull;
    for (; index < text.size(); ++index) {
        const char c = text[index];
        if (c < '0' || c > '9') throw_decoding("expected an Int");
        accumulator = accumulator * 10 + static_cast<std::uint64_t>(c - '0');
        if (accumulator > limit) throw_decoding("expected an Int");
    }
    if (negative) {
        if (accumulator == 2147483648ull) return INT32_MIN;
        return -static_cast<std::int32_t>(accumulator);
    }
    return static_cast<std::int32_t>(accumulator);
}

std::optional<std::int32_t> decode_optional_int(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_int_value(*value);
}

/**
 * Float decode: the strict JSON number lexeme only (quoted decimals accepted, the
 * "Infinity"/"NaN" spellings rejected), and a non-finite result is a decode error (pinned by the
 * golden traces).
 */
float decode_float_value(const json::Value& value) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        throw_decoding("expected a JSON number");
    }
    if (!is_json_number_lexeme(value.text)) throw_decoding("expected a Float");
    const std::string& text = value.text;
    const char* begin = text.c_str();
    char* end = nullptr;
    const float parsed = std::strtof(begin, &end);
    if (end != begin + text.size()) throw_decoding("expected a Float");
    if (!std::isfinite(parsed)) throw_decoding("expected a Float");
    return parsed;
}

std::optional<float> decode_optional_float(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_float_value(*value);
}

/** Kotlin toBooleanStrict() on JsonPrimitive.content: only the two spellings, quoted or not. */
bool decode_bool_value(const json::Value& value) {
    if (value.kind != json::Kind::Literal && value.kind != json::Kind::String) {
        throw_decoding("expected a JSON boolean");
    }
    if (value.text == "true") return true;
    if (value.text == "false") return false;
    throw_decoding("expected a JSON boolean");
}

std::optional<bool> decode_optional_bool(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_bool_value(*value);
}

const json::Value& require_object(const json::Value* value, std::string_view what) {
    if (value == nullptr || is_null(value)) throw_decoding(std::string(what) + ": unexpected null");
    if (value->kind != json::Kind::Object) throw_decoding(std::string(what) + ": expected an object");
    return *value;
}

// ---------------------------------------------------------------------------------------------
// Signature compatibility (AnthropicProvider.kt private funs)
// ---------------------------------------------------------------------------------------------

bool signature_is_compatible_with_anthropic(
    const prep::MessageSegment& segment,
    const std::optional<std::string>& source_model,
    const std::string& target_model,
    const std::string& target_provider_name) {
    if (!source_model) return false;
    const std::string trimmed = json::trim(*source_model);
    if (trimmed.empty()) return false;
    const ParsedModelId stored = parse_model_id(trimmed);
    std::string issuer = stored.provider_name;
    if (segment.signature_provider) {
        const std::string candidate = json::trim(*segment.signature_provider);
        if (!candidate.empty()) issuer = candidate;
    }
    if (!equals_ignore_ascii(issuer, target_provider_name)) return false;
    return equals_ignore_ascii(strip_models_prefix(stored.model_name),
                               strip_models_prefix(json::trim(target_model)));
}

bool is_anthropic_tool_round_compatible(const prep::ChatMessage& message,
                                        const std::string& target_model,
                                        const std::string& target_provider_name,
                                        bool signed_thinking_required) {
    if (!signed_thinking_required) return true;
    std::vector<const prep::MessageSegment*> thoughts;
    if (message.segments) {
        for (const prep::MessageSegment& segment : *message.segments) {
            if (segment.type == "thought" && !json::is_blank(segment.content)) {
                thoughts.push_back(&segment);
            }
        }
    }
    if (thoughts.empty()) return false;
    for (const prep::MessageSegment* segment : thoughts) {
        if (!segment->signature || json::is_blank(*segment->signature)) return false;
        if (!signature_is_compatible_with_anthropic(*segment, message.model_name, target_model,
                                                   target_provider_name)) {
            return false;
        }
    }
    return true;
}

// ---------------------------------------------------------------------------------------------
// JSON serialization — kotlinx with encodeDefaults=true, explicitNulls=false.
// ---------------------------------------------------------------------------------------------

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

/** kotlinx Json encoder for a Float: non-finite values are rejected before formatting. */
void append_float(std::string& out, float value) {
    if (!std::isfinite(value)) {
        throw ProviderRequestError{
            "JsonEncodingException",
            "Unexpected special floating-point value " + openai::format_kotlin_float(value) +
                ". By default, non-finite floating point values are prohibited because they do "
                "not conform to JSON."};
    }
    out += openai::format_kotlin_float(value);
}

void write_cache_control(std::string& out, const AnthropicCacheControl& cache_control) {
    out += "{\"type\":";
    append_quoted(out, cache_control.type);
    out += ",\"ttl\":";
    append_quoted(out, cache_control.ttl);
    out += "}";
}

void write_tool(std::string& out, const AnthropicTool& tool) {
    out += "{\"name\":";
    append_quoted(out, tool.name);
    out += ",\"description\":";
    append_quoted(out, tool.description);
    out += ",\"input_schema\":";
    out += json::to_kotlin_string(tool.input_schema);
    out += "}";
}

void write_thinking(std::string& out, const AnthropicThinking& thinking) {
    out += "{\"type\":";
    append_quoted(out, thinking.type);
    if (thinking.budget_tokens) {
        out += ",\"budget_tokens\":" + std::to_string(*thinking.budget_tokens);
    }
    if (thinking.display) {
        out += ",\"display\":";
        append_quoted(out, *thinking.display);
    }
    out += "}";
}

void write_output_config(std::string& out, const AnthropicOutputConfig& output_config) {
    out += "{\"effort\":";
    append_quoted(out, output_config.effort);
    out += "}";
}

void write_image_source(std::string& out, const AnthropicImageSource& source) {
    out += "{\"type\":";
    append_quoted(out, source.type);
    out += ",\"media_type\":";
    append_quoted(out, source.media_type);
    out += ",\"data\":";
    append_quoted(out, source.data);
    out += "}";
}

void write_content_part(std::string& out, const AnthropicContentPart& part) {
    out += "{\"type\":";
    append_quoted(out, part.type);
    if (part.text) {
        out += ",\"text\":";
        append_quoted(out, *part.text);
    }
    if (part.thinking) {
        out += ",\"thinking\":";
        append_quoted(out, *part.thinking);
    }
    if (part.signature) {
        out += ",\"signature\":";
        append_quoted(out, *part.signature);
    }
    if (part.source) {
        out += ",\"source\":";
        write_image_source(out, *part.source);
    }
    if (part.id) {
        out += ",\"id\":";
        append_quoted(out, *part.id);
    }
    if (part.name) {
        out += ",\"name\":";
        append_quoted(out, *part.name);
    }
    if (part.input) {
        out += ",\"input\":";
        out += json::to_kotlin_string(*part.input);
    }
    if (part.tool_use_id) {
        out += ",\"tool_use_id\":";
        append_quoted(out, *part.tool_use_id);
    }
    if (part.content) {
        out += ",\"content\":";
        append_quoted(out, *part.content);
    }
    out += "}";
}

void write_message_body(std::string& out, const AnthropicMessage& message) {
    out += "{\"role\":";
    append_quoted(out, message.role);
    out += ",\"content\":[";
    for (std::size_t i = 0; i < message.content.size(); ++i) {
        if (i > 0) out += ",";
        write_content_part(out, message.content[i]);
    }
    out += "]}";
}

void write_request_body(std::string& out, const AnthropicRequest& request) {
    out += "{\"model\":";
    append_quoted(out, request.model);
    out += ",\"messages\":[";
    for (std::size_t i = 0; i < request.messages.size(); ++i) {
        if (i > 0) out += ",";
        write_message_body(out, request.messages[i]);
    }
    out += "]";
    if (request.system) {
        out += ",\"system\":";
        append_quoted(out, *request.system);
    }
    // encodeDefaults=true: max_tokens and stream always serialize.
    out += ",\"max_tokens\":" + std::to_string(request.max_tokens);
    out += ",\"stream\":";
    out += request.stream ? "true" : "false";
    if (request.thinking) {
        out += ",\"thinking\":";
        write_thinking(out, *request.thinking);
    }
    if (request.output_config) {
        out += ",\"output_config\":";
        write_output_config(out, *request.output_config);
    }
    if (request.tools) {
        out += ",\"tools\":[";
        for (std::size_t i = 0; i < request.tools->size(); ++i) {
            if (i > 0) out += ",";
            write_tool(out, (*request.tools)[i]);
        }
        out += "]";
    }
    if (request.cache_control) {
        out += ",\"cache_control\":";
        write_cache_control(out, *request.cache_control);
    }
    if (request.temperature) {
        out += ",\"temperature\":";
        append_float(out, *request.temperature);
    }
    if (request.top_p) {
        out += ",\"top_p\":";
        append_float(out, *request.top_p);
    }
    out += "}";
}

// ---------------------------------------------------------------------------------------------
// Pipeline constants — AnthropicProvider companion object.
// ---------------------------------------------------------------------------------------------

constexpr std::int32_t kAnswerHeadroomTokens = 8192;
constexpr std::int32_t kDefaultMaxTokens = 32768;
constexpr std::string_view kToolMsgPrefix = "tool_";
constexpr std::string_view kResultMsgPrefix = "result_";

}  // namespace

// ---------------------------------------------------------------------------------------------
// AnthropicOutputLimits.maxOutputTokens
// ---------------------------------------------------------------------------------------------

std::optional<std::int32_t> max_output_tokens(std::string_view model_id) {
    struct Ceiling {
        std::string_view prefix;
        std::int32_t value;
    };
    // Ordered so that a longer, more specific prefix always wins over a shorter one.
    static constexpr Ceiling kCeilings[] = {
        {"claude-fable-5", 128000},
        {"claude-mythos-5", 128000},
        {"claude-opus-5", 128000},
        {"claude-sonnet-5", 128000},
        {"claude-opus-4-8", 128000},
        {"claude-opus-4-7", 128000},
        {"claude-opus-4-6", 128000},
        {"claude-sonnet-4-6", 128000},
        {"claude-opus-4-5", 64000},
        {"claude-sonnet-4-5", 64000},
        {"claude-haiku-4-5", 64000},
        {"claude-opus-4-1", 32000},
        {"claude-opus-4", 32000},
        {"claude-sonnet-4", 64000},
        {"claude-3-7-sonnet", 64000},
        {"claude-3-5-sonnet", 8192},
        {"claude-3-5-haiku", 8192},
        {"claude-3-opus", 4096},
        {"claude-3-sonnet", 4096},
        {"claude-3-haiku", 4096},
    };

    std::string model = lower_ascii(json::trim(model_id));
    const std::size_t slash = model.find_last_of('/');
    if (slash != std::string::npos) model = model.substr(slash + 1);
    // replace('.', '-'): dotted versions hit the same ceiling entries.
    std::replace(model.begin(), model.end(), '.', '-');

    for (const Ceiling& ceiling : kCeilings) {
        if (model == ceiling.prefix || starts_with(model, std::string(ceiling.prefix) + "-") ||
            starts_with(model, std::string(ceiling.prefix) + "@")) {
            return ceiling.value;
        }
    }
    return std::nullopt;
}

// ---------------------------------------------------------------------------------------------
// Message conversion
// ---------------------------------------------------------------------------------------------

AnthropicContentPart build_tool_use_block(const std::optional<std::string>& id,
                                          const std::optional<std::string>& name,
                                          const std::optional<std::string>& args) {
    const std::string tool_id =
        id ? *id : openai::build_tool_call_id(name.value_or(""), args.value_or("{}"), "tool_");
    AnthropicContentPart part;
    part.type = "tool_use";
    part.id = tool_id;
    part.name = name.value_or("");
    // parseToJsonElement(args ?: "{}") as? JsonObject ?: JsonObject(emptyMap())
    json::Value input;
    input.kind = json::Kind::Object;
    const json::ParseResult parsed = json::parse(args.value_or("{}"));
    if (parsed.value && parsed.value->kind == json::Kind::Object) input = *parsed.value;
    part.input = std::move(input);
    return part;
}

std::vector<AnthropicContentPart> build_tool_result_blocks(const prep::ChatMessage& msg) {
    std::vector<const prep::MessageSegment*> tool_segments;
    if (msg.segments) {
        for (const prep::MessageSegment& segment : *msg.segments) {
            if (segment.type == "tool") tool_segments.push_back(&segment);
        }
    }
    std::vector<AnthropicContentPart> blocks;
    if (!tool_segments.empty()) {
        for (const prep::MessageSegment* segment : tool_segments) {
            AnthropicContentPart part;
            part.type = "tool_result";
            part.tool_use_id =
                segment->tool_call_id
                    ? *segment->tool_call_id
                    : openai::build_tool_call_id(segment->tool_name.value_or(""),
                                                 segment->tool_args.value_or("{}"), "tool_");
            part.content = segment->tool_result.value_or("");
            blocks.push_back(std::move(part));
        }
        return blocks;
    }
    if (!msg.tool_call) return blocks;
    const prep::ToolCallData& call = *msg.tool_call;
    AnthropicContentPart part;
    part.type = "tool_result";
    part.tool_use_id = call.tool_call_id
                           ? *call.tool_call_id
                           : openai::build_tool_call_id(call.tool_name, call.arguments, "tool_");
    part.content = call.result;
    blocks.push_back(std::move(part));
    return blocks;
}

AnthropicMessage build_assistant_tool_use(const prep::ChatMessage& msg,
                                          const std::string& target_model,
                                          const std::string& provider_name) {
    std::vector<AnthropicContentPart> parts;
    // Signed thinking replay: every non-blank thought segment whose signature is attributable
    // to the target model and provider (the gate is the signature itself, not the thinking
    // toggle).
    if (msg.segments) {
        for (const prep::MessageSegment& segment : *msg.segments) {
            if (segment.type != "thought" || segment.content.empty()) continue;
            if (!segment.signature || json::is_blank(*segment.signature)) continue;
            if (!signature_is_compatible_with_anthropic(segment, msg.model_name, target_model,
                                                        provider_name)) {
                continue;
            }
            AnthropicContentPart part;
            part.type = "thinking";
            part.thinking = segment.content;
            part.signature = segment.signature;
            parts.push_back(std::move(part));
        }
    }

    std::vector<const prep::MessageSegment*> tool_segments;
    if (msg.segments) {
        for (const prep::MessageSegment& segment : *msg.segments) {
            if (segment.type == "tool") tool_segments.push_back(&segment);
        }
    }
    if (!tool_segments.empty()) {
        for (const prep::MessageSegment* segment : tool_segments) {
            parts.push_back(
                build_tool_use_block(segment->tool_call_id, segment->tool_name, segment->tool_args));
        }
        return AnthropicMessage{"assistant", std::move(parts)};
    }

    if (!msg.tool_call) {
        // The early return is tool_use-only: signed thoughts collected above are discarded.
        AnthropicContentPart text;
        text.type = "text";
        text.text = "Continue";
        return AnthropicMessage{"assistant", {std::move(text)}};
    }
    const prep::ToolCallData& call = *msg.tool_call;
    parts.push_back(build_tool_use_block(call.tool_call_id, call.tool_name, call.arguments));
    return AnthropicMessage{"assistant", std::move(parts)};
}

AnthropicMessage build_normal_message(const prep::ChatMessage& msg,
                                      openai::Base64FileRegistry& base64_files) {
    std::vector<AnthropicContentPart> parts;
    std::vector<std::string> image_paths;
    if (msg.participant == prep::Participant::User) image_paths = msg.images;
    for (const std::string& image_path : image_paths) {
        const std::optional<std::string> placeholder = base64_files.register_file(image_path);
        if (!placeholder) continue;
        AnthropicContentPart part;
        part.type = "image";
        part.source = AnthropicImageSource{"base64", openai::image_mime_type(image_path),
                                           *placeholder};
        parts.push_back(std::move(part));
    }
    // isNotBlank, NOT isNotEmpty: Anthropic rejects whitespace-only text blocks.
    if (!json::is_blank(msg.text)) {
        AnthropicContentPart part;
        part.type = "text";
        part.text = msg.text;
        parts.push_back(std::move(part));
    }
    if (parts.empty()) {
        AnthropicContentPart part;
        part.type = "text";
        part.text = "Continue";
        parts.push_back(std::move(part));
    }
    const std::string role =
        msg.participant == prep::Participant::User ? "user" : "assistant";
    return AnthropicMessage{role, std::move(parts)};
}

std::vector<AnthropicMessage> coalesce_anthropic_messages(
    const std::vector<AnthropicMessage>& messages) {
    std::vector<AnthropicMessage> result;
    for (const AnthropicMessage& message : messages) {
        if (!result.empty() && result.back().role == message.role) {
            result.back().content.insert(result.back().content.end(), message.content.begin(),
                                         message.content.end());
        } else {
            result.push_back(message);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Validation gate — AnthropicRequest.requireValidWireFormat (the Kotlin extension hardcodes the
// "Anthropic" provider string into its exception text).
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const AnthropicRequest& request) {
    std::vector<std::string> violations;

    if (json::is_blank(request.model)) violations.push_back("model is blank");
    if (request.messages.empty()) violations.push_back("messages is empty");
    if (request.system && json::is_blank(*request.system)) violations.push_back("system is blank");
    if (request.max_tokens <= 0) violations.push_back("max_tokens must be positive");
    if (request.top_p && !(*request.top_p >= 0.0f && *request.top_p <= 1.0f)) {
        violations.push_back("top_p is outside 0..1");
    }

    // validateThinking
    if (!request.thinking) {
        if (request.output_config) violations.push_back("output_config requires adaptive thinking");
    } else {
        const AnthropicThinking& thinking = *request.thinking;
        if (thinking.type != "enabled" && thinking.type != "adaptive" &&
            thinking.type != "disabled") {
            violations.push_back("thinking.type is invalid");
        }
        if (thinking.type == "enabled" && (!thinking.budget_tokens || *thinking.budget_tokens < 1024)) {
            violations.push_back("enabled thinking requires budget_tokens >= 1024");
        }
        if (thinking.type == "adaptive" && thinking.budget_tokens) {
            violations.push_back("adaptive thinking cannot carry budget_tokens");
        }
        if (thinking.type == "disabled" && (thinking.budget_tokens || thinking.display)) {
            violations.push_back("disabled thinking cannot carry budget_tokens or display");
        }
        if (request.output_config && thinking.type != "adaptive" && thinking.type != "disabled") {
            violations.push_back("output_config requires adaptive or disabled thinking");
        }
    }

    // validateTools
    if (request.tools) {
        std::vector<std::string> names;
        for (std::size_t index = 0; index < request.tools->size(); ++index) {
            const AnthropicTool& tool = (*request.tools)[index];
            const std::string location = "tools[" + std::to_string(index) + "]";
            if (!wire_safe(tool.name, 64)) violations.push_back(location + ".name is not wire-safe");
            if (!add_unique(names, tool.name)) {
                violations.push_back("duplicate tool name " + tool.name);
            }
            // inputSchema["type"]?.toString()?.trim('"') != "object"
            const json::Value* schema_type =
                tool.input_schema.kind == json::Kind::Object ? member(tool.input_schema, "type")
                                                            : nullptr;
            std::string rendered =
                schema_type ? json::to_kotlin_string(*schema_type) : std::string();
            const std::size_t first = rendered.find_first_not_of('"');
            if (first == std::string::npos) {
                rendered.clear();
            } else {
                const std::size_t last = rendered.find_last_not_of('"');
                rendered = rendered.substr(first, last - first + 1);
            }
            if (rendered != "object") {
                violations.push_back("tool " + tool.name + " input_schema is not an object schema");
            }
        }
    }

    std::vector<std::string> seen_tool_ids;
    std::vector<std::string> pending_tool_ids;
    std::optional<std::string> previous_role;

    for (std::size_t i = 0; i < request.messages.size(); ++i) {
        const AnthropicMessage& message = request.messages[i];
        const std::string location = "messages[" + std::to_string(i) + "]";

        if (message.role != "user" && message.role != "assistant") {
            violations.push_back(location + " has invalid role " + message.role);
        }
        if (previous_role && *previous_role == message.role) {
            violations.push_back(location + " repeats role " + message.role);
        }
        previous_role = message.role;
        if (message.content.empty()) violations.push_back(location + " content is empty");

        std::size_t leading_results = 0;
        while (leading_results < message.content.size() &&
               message.content[leading_results].type == "tool_result") {
            ++leading_results;
        }
        bool later_results = false;
        for (std::size_t j = leading_results; j < message.content.size(); ++j) {
            if (message.content[j].type == "tool_result") {
                later_results = true;
                break;
            }
        }
        if (later_results) {
            violations.push_back(location + " has tool_result after another content type");
        }

        std::size_t leading_thinking = 0;
        while (leading_thinking < message.content.size() &&
               message.content[leading_thinking].type == "thinking") {
            ++leading_thinking;
        }
        bool later_thinking = false;
        for (std::size_t j = leading_thinking; j < message.content.size(); ++j) {
            if (message.content[j].type == "thinking") {
                later_thinking = true;
                break;
            }
        }
        if (later_thinking) {
            violations.push_back(location + " has thinking after another content type");
        }

        const bool thinking_on =
            request.thinking &&
            (request.thinking->type == "enabled" || request.thinking->type == "adaptive");
        if (thinking_on && leading_thinking == 0) {
            bool has_tool_use = false;
            for (const AnthropicContentPart& part : message.content) {
                if (part.type == "tool_use") {
                    has_tool_use = true;
                    break;
                }
            }
            if (has_tool_use) {
                violations.push_back(
                    location + " tool_use is missing its leading signed thinking block");
            }
        }

        if (!pending_tool_ids.empty()) {
            if (message.role != "user") {
                violations.push_back(location + " does not immediately answer pending tool_use blocks");
            }
            std::vector<std::string> returned_ids;
            for (std::size_t j = 0; j < leading_results; ++j) {
                if (message.content[j].tool_use_id) {
                    returned_ids.push_back(*message.content[j].tool_use_id);
                }
            }
            const bool same_size = returned_ids.size() == pending_tool_ids.size();
            const bool distinct = [&] {
                std::vector<std::string> unique;
                for (const std::string& id : returned_ids) add_unique(unique, id);
                return unique.size() == returned_ids.size();
            }();
            const bool same_set = same_elements(returned_ids, pending_tool_ids);
            if (!same_size || !distinct || !same_set) {
                violations.push_back(location +
                                     " does not answer every pending tool_use exactly once");
            }
            pending_tool_ids.clear();
        } else if (leading_results > 0) {
            violations.push_back(location + " has orphan tool_result blocks");
        }

        for (std::size_t j = 0; j < message.content.size(); ++j) {
            const AnthropicContentPart& part = message.content[j];
            const std::string part_location = location + ".content[" + std::to_string(j) + "]";

            int populated = 0;
            if (part.text) ++populated;
            if (part.thinking) ++populated;
            if (part.signature) ++populated;
            if (part.source) ++populated;
            if (part.id) ++populated;
            if (part.name) ++populated;
            if (part.input) ++populated;
            if (part.tool_use_id) ++populated;
            if (part.content) ++populated;

            if (part.type == "text") {
                if (!part.text || json::is_blank(*part.text) || populated != 1) {
                    violations.push_back(part_location + " is not a valid nonblank text block");
                }
            } else if (part.type == "image") {
                if (message.role != "user" || !part.source ||
                    json::is_blank(part.source->media_type) ||
                    json::is_blank(part.source->data) || populated != 1) {
                    violations.push_back(part_location + " is not a valid user image block");
                }
            } else if (part.type == "thinking") {
                if (message.role != "assistant" || !part.thinking ||
                    json::is_blank(*part.thinking) || !part.signature ||
                    json::is_blank(*part.signature) || populated != 2) {
                    violations.push_back(part_location + " is not a valid signed thinking block");
                }
            } else if (part.type == "tool_use") {
                if (message.role != "assistant" || !part.id || !wire_safe(*part.id, 128) ||
                    !part.name || !wire_safe(*part.name, 64) || !part.input || populated != 3) {
                    violations.push_back(part_location + " is not a valid tool_use block");
                }
                if (part.id && !json::is_blank(*part.id)) {
                    if (!add_unique(seen_tool_ids, *part.id)) {
                        violations.push_back(part_location + " reuses tool_use id " + *part.id);
                    }
                    if (!add_unique(pending_tool_ids, *part.id)) {
                        violations.push_back(part_location + " duplicates tool_use id " + *part.id);
                    }
                }
            } else if (part.type == "tool_result") {
                if (message.role != "user" || !part.tool_use_id ||
                    json::is_blank(*part.tool_use_id) || !part.content || populated != 2) {
                    violations.push_back(part_location + " is not a valid tool_result block");
                }
            } else {
                violations.push_back(part_location + " has unsupported type " + part.type);
            }
        }
    }

    if (!pending_tool_ids.empty()) {
        violations.push_back("tool_use blocks are missing tool_result blocks");
    }
    if (request.messages.empty() || request.messages.front().role != "user") {
        violations.push_back("history does not start with user");
    }
    if (request.messages.empty() || request.messages.back().role != "user") {
        violations.push_back("history does not end with user input");
    }

    if (!violations.empty()) throw_request_format("Anthropic", violations);
}

// ---------------------------------------------------------------------------------------------
// JSON writing — public list/message writers.
// ---------------------------------------------------------------------------------------------

std::string write_anthropic_message(const AnthropicMessage& message) {
    std::string out;
    write_message_body(out, message);
    return out;
}

std::string write_anthropic_message_list(const std::vector<AnthropicMessage>& messages) {
    std::string out;
    out += "[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) out += ",";
        write_message_body(out, messages[i]);
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------------------------
// The pipeline — AnthropicProvider.buildRequestBody and its surroundings.
// ---------------------------------------------------------------------------------------------

BuiltAnthropicRequest build_anthropic_request(const std::string& provider_name,
                                              const openai::ProviderConfig& config,
                                              const ProviderRequestInput& input) {
    // resolvedThinking(ThinkingProviderFamily.ANTHROPIC)
    const openai::ResolvedThinking resolved = openai::resolve_thinking(
        openai::capability_for_model(openai::ThinkingProviderFamily::Anthropic, config.model_id),
        config.thinking_enabled, config.thinking_level, config.thinking_budget_enabled,
        config.thinking_budget_tokens);
    const openai::ModelThinkingCapability& capability = resolved.capability;

    const bool has_thinking_controls =
        capability.supports_effort() || capability.supports_thinking_budget;
    std::optional<AnthropicThinking> thinking;
    if (!has_thinking_controls) {
        thinking = std::nullopt;
    } else if (resolved.disabled()) {
        thinking = AnthropicThinking{"disabled", std::nullopt, std::nullopt};
    } else if (resolved.budget_tokens) {
        thinking = AnthropicThinking{"enabled", *resolved.budget_tokens, "summarized"};
    } else if (capability.supports_effort()) {
        thinking = AnthropicThinking{"adaptive", std::nullopt, "summarized"};
    } else {
        thinking = AnthropicThinking{"enabled",
                                     capability.clamp_budget(config.thinking_budget_tokens),
                                     "summarized"};
    }

    // effort is an output control: sent whenever the model accepts one, thinking off included.
    std::optional<std::string> effort = resolved.effort;
    if (!effort) effort = capability.nearest_effort(config.thinking_level);
    std::optional<AnthropicOutputConfig> output_config;
    if (effort) output_config = AnthropicOutputConfig{*effort};

    std::optional<std::vector<AnthropicTool>> anthropic_tools;
    if (config.tools) {
        std::vector<AnthropicTool> tools;
        tools.reserve(config.tools->size());
        for (const openai::ToolDefinition& definition : *config.tools) {
            tools.push_back(AnthropicTool{definition.function.name, definition.function.description,
                                          openai::tool_schema_json_of(
                                              definition.function.parameters)});
        }
        anthropic_tools = std::move(tools);
    }

    // The cache gate fires before anything is converted.
    if (config.anthropic_cache_enabled && config.anthropic_cache_ttl != "5m" &&
        config.anthropic_cache_ttl != "1h") {
        throw ProviderRequestError{
            "RequestFormatException",
            provider_name + " request validation failed: Invalid Anthropic cache duration"};
    }

    const bool signed_thinking_required =
        thinking && (thinking->type == "enabled" || thinking->type == "adaptive");
    const std::vector<prep::ChatMessage> validated_path = prep::adapt_tool_rounds_for_provider(
        input.messages, provider_name, [&](const prep::ChatMessage& tool_message) {
            return is_anthropic_tool_round_compatible(tool_message, config.model_id, provider_name,
                                                      signed_thinking_required);
        });

    // Message walk: tool_ row (+ its result_ batch), orphan result_ rows are dropped, normal
    // messages carry their images unless the config disables them.
    openai::Base64FileRegistry base64_files;
    std::vector<AnthropicMessage> api_messages;
    std::size_t index = 0;
    while (index < validated_path.size()) {
        const prep::ChatMessage& message = validated_path[index];
        if (starts_with(message.id, kToolMsgPrefix)) {
            api_messages.push_back(build_assistant_tool_use(message, config.model_id, provider_name));
            ++index;
            if (index < validated_path.size() &&
                starts_with(validated_path[index].id, kResultMsgPrefix)) {
                std::vector<AnthropicContentPart> result_blocks;
                while (index < validated_path.size() &&
                       starts_with(validated_path[index].id, kResultMsgPrefix)) {
                    std::vector<AnthropicContentPart> blocks =
                        build_tool_result_blocks(validated_path[index]);
                    result_blocks.insert(result_blocks.end(), blocks.begin(), blocks.end());
                    ++index;
                }
                api_messages.push_back(AnthropicMessage{"user", std::move(result_blocks)});
            }
        } else if (starts_with(message.id, kResultMsgPrefix)) {
            ++index;
        } else {
            prep::ChatMessage normal = message;
            if (!config.include_images) normal.images.clear();
            api_messages.push_back(build_normal_message(normal, base64_files));
            ++index;
        }
    }
    api_messages = coalesce_anthropic_messages(api_messages);

    // max_tokens: an explicit value wins; otherwise the default (raised above a thinking budget)
    // capped by the model's documented ceiling.
    std::int32_t max_tokens = 0;
    if (config.max_tokens) {
        max_tokens = *config.max_tokens;
    } else {
        std::int32_t wanted = kDefaultMaxTokens;
        if (thinking && thinking->budget_tokens) {
            const std::int32_t wrapped = static_cast<std::int32_t>(
                static_cast<std::uint32_t>(*thinking->budget_tokens) +
                static_cast<std::uint32_t>(kAnswerHeadroomTokens));
            wanted = std::max(wrapped, kDefaultMaxTokens);
        }
        const std::optional<std::int32_t> ceiling = max_output_tokens(config.model_id);
        max_tokens = ceiling ? std::min(wanted, *ceiling) : wanted;
    }

    AnthropicRequest request;
    request.model = config.model_id;
    request.messages = std::move(api_messages);
    request.system = input.system_prompt;
    request.max_tokens = max_tokens;
    request.stream = true;
    request.thinking = thinking;
    request.output_config = output_config;
    request.tools = anthropic_tools;
    request.cache_control =
        config.anthropic_cache_enabled
            ? std::optional<AnthropicCacheControl>(
                  AnthropicCacheControl{"ephemeral", config.anthropic_cache_ttl})
            : std::nullopt;
    // Sampling parameters: only when the model documents them AND no thinking parameter is
    // present at all; top_p additionally stays legal in 0.95..1 with thinking.
    request.temperature =
        (config.temperature && capability.supports_sampling_params && !thinking)
            ? config.temperature
            : std::nullopt;
    request.top_p =
        (config.top_p && capability.supports_sampling_params &&
         (!thinking || (*config.top_p >= 0.95f && *config.top_p <= 1.0f)))
            ? config.top_p
            : std::nullopt;

    // The provider order: wire gate, encode, base64 preparation, serialized-body gate.
    require_valid_wire_format(request);

    std::string body_json;
    write_request_body(body_json, request);

    std::optional<openai::StreamingJsonBody> streaming_body =
        base64_files.prepare(body_json);

    openai::require_valid_serialized_request(provider_name, body_json, {"model"}, {"messages"});

    BuiltAnthropicRequest built;
    built.api_messages = std::move(request.messages);
    built.body_json = std::move(body_json);
    built.streaming_body = std::move(streaming_body);
    return built;
}

// ---------------------------------------------------------------------------------------------
// Serializable decode — Json { ignoreUnknownKeys = true }.decodeFromJsonElement(...) rules:
// unknown keys skipped, duplicates last-wins, explicit nulls are null, defaults from the
// property declarations.
// ---------------------------------------------------------------------------------------------

AnthropicRequest decode_anthropic_request(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decoding("expected an object");

    AnthropicRequest request;

    const json::Value* model = member(value, "model");
    if (model == nullptr) throw_missing_field("model");
    request.model = decode_string(*model);

    const json::Value* messages = member(value, "messages");
    if (messages == nullptr) throw_missing_field("messages");
    if (is_null(messages)) throw_decoding("unexpected null");
    if (messages->kind != json::Kind::Array) throw_decoding("expected an array");
    for (const json::Value& entry : messages->elements) {
        if (entry.kind != json::Kind::Object) throw_decoding("expected an object");
        AnthropicMessage message;
        const json::Value* role = member(entry, "role");
        if (role == nullptr) throw_missing_field("role");
        message.role = decode_string(*role);
        const json::Value* content = member(entry, "content");
        if (content == nullptr) throw_missing_field("content");
        if (is_null(content)) throw_decoding("unexpected null");
        if (content->kind != json::Kind::Array) throw_decoding("expected an array");
        for (const json::Value& part_entry : content->elements) {
            if (part_entry.kind != json::Kind::Object) throw_decoding("expected an object");
            AnthropicContentPart part;
            const json::Value* type = member(part_entry, "type");
            if (type == nullptr) throw_missing_field("type");
            part.type = decode_string(*type);
            part.text = decode_optional_string(part_entry, "text");
            part.thinking = decode_optional_string(part_entry, "thinking");
            part.signature = decode_optional_string(part_entry, "signature");
            if (const json::Value* source = member(part_entry, "source");
                source != nullptr && !is_null(source)) {
                if (source->kind != json::Kind::Object) throw_decoding("expected an object");
                AnthropicImageSource decoded_source;
                decoded_source.type =
                    decode_optional_string(*source, "type").value_or("base64");
                const json::Value* media_type = member(*source, "media_type");
                if (media_type == nullptr) throw_missing_field("media_type");
                decoded_source.media_type = decode_string(*media_type);
                const json::Value* data = member(*source, "data");
                if (data == nullptr) throw_missing_field("data");
                decoded_source.data = decode_string(*data);
                part.source = std::move(decoded_source);
            }
            part.id = decode_optional_string(part_entry, "id");
            part.name = decode_optional_string(part_entry, "name");
            if (const json::Value* input = member(part_entry, "input");
                input != nullptr && !is_null(input)) {
                if (input->kind != json::Kind::Object) throw_decoding("expected an object");
                part.input = *input;
            }
            part.tool_use_id = decode_optional_string(part_entry, "tool_use_id");
            part.content = decode_optional_string(part_entry, "content");
            message.content.push_back(std::move(part));
        }
        request.messages.push_back(std::move(message));
    }

    request.system = decode_optional_string(value, "system");
    if (const auto max_tokens = decode_optional_int(value, "max_tokens")) {
        request.max_tokens = *max_tokens;
    }
    if (const auto stream = decode_optional_bool(value, "stream")) request.stream = *stream;

    if (const json::Value* thinking = member(value, "thinking");
        thinking != nullptr && !is_null(thinking)) {
        if (thinking->kind != json::Kind::Object) throw_decoding("expected an object");
        AnthropicThinking decoded;
        decoded.type = decode_optional_string(*thinking, "type").value_or("enabled");
        decoded.budget_tokens = decode_optional_int(*thinking, "budget_tokens");
        decoded.display = decode_optional_string(*thinking, "display");
        request.thinking = std::move(decoded);
    }
    if (const json::Value* output_config = member(value, "output_config");
        output_config != nullptr && !is_null(output_config)) {
        require_object(output_config, "output_config");
        AnthropicOutputConfig decoded;
        const json::Value* effort = member(*output_config, "effort");
        if (effort == nullptr) throw_missing_field("effort");
        decoded.effort = decode_string(*effort);
        request.output_config = std::move(decoded);
    }
    if (const json::Value* tools = member(value, "tools");
        tools != nullptr && !is_null(tools)) {
        if (tools->kind != json::Kind::Array) throw_decoding("expected an array");
        std::vector<AnthropicTool> decoded;
        for (const json::Value& entry : tools->elements) {
            if (entry.kind != json::Kind::Object) throw_decoding("expected an object");
            AnthropicTool tool;
            const json::Value* name = member(entry, "name");
            if (name == nullptr) throw_missing_field("name");
            tool.name = decode_string(*name);
            const json::Value* description = member(entry, "description");
            if (description == nullptr) throw_missing_field("description");
            tool.description = decode_string(*description);
            const json::Value* input_schema = member(entry, "input_schema");
            if (input_schema == nullptr) throw_missing_field("input_schema");
            if (input_schema->kind != json::Kind::Object) throw_decoding("expected an object");
            tool.input_schema = *input_schema;
            decoded.push_back(std::move(tool));
        }
        request.tools = std::move(decoded);
    }
    if (const json::Value* cache_control = member(value, "cache_control");
        cache_control != nullptr && !is_null(cache_control)) {
        require_object(cache_control, "cache_control");
        AnthropicCacheControl decoded;
        decoded.type = decode_optional_string(*cache_control, "type").value_or("ephemeral");
        decoded.ttl = decode_optional_string(*cache_control, "ttl").value_or("1h");
        request.cache_control = decoded;
    }

    request.temperature = decode_optional_float(value, "temperature");
    request.top_p = decode_optional_float(value, "top_p");

    return request;
}

}  // namespace agora::core::anthropic
