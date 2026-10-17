#include "agora/core/gemini_request.hpp"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"

namespace agora::core::gemini {
namespace {

namespace json = agora::core::json;
namespace openai = agora::core::openai;
namespace prep = agora::core::prep;

// ---------------------------------------------------------------------------------------------
// String helpers (Kotlin semantics as pinned by the shared modules)
// ---------------------------------------------------------------------------------------------

std::string lower_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'A' && c <= 'Z') c = static_cast<char>(c - 'A' + 'a');
    }
    return text;
}

std::string upper_ascii(std::string text) {
    for (char& c : text) {
        if (c >= 'a' && c <= 'z') c = static_cast<char>(c - 'a' + 'A');
    }
    return text;
}

bool starts_with(std::string_view value, std::string_view prefix) {
    return value.size() >= prefix.size() && value.substr(0, prefix.size()) == prefix;
}

/** String.contains(other, ignoreCase = true) for ASCII patterns. */
bool contains_ignore_ascii(std::string_view haystack, std::string_view needle) {
    const std::string folded_haystack = lower_ascii(std::string(haystack));
    const std::string folded_needle = lower_ascii(std::string(needle));
    return folded_haystack.find(folded_needle) != std::string::npos;
}

/** String.equals(ignoreCase = true) for the ASCII wire-identity grammar. */
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

/** Regex("[A-Za-z0-9_-]{1,N}") matches(): ASCII grammar, byte counts equal UTF-16 units. */
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
// ModelId.parse — verbatim semantics (case-sensitive legacy heuristics), as in the Anthropic
// request module: the two modules pin the same Kotlin function independently.
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
 * The number grammar kotlinx applies to numeric content: -?digits(.digits)?(e[+-]?digits)?
 * (leading zeros accepted — pinned by the golden traces).
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

// ---------------------------------------------------------------------------------------------
// JsonObject.stringContent: a non-blank primitive's content.
// ---------------------------------------------------------------------------------------------

std::optional<std::string> primitive_string_content(const json::Value& object,
                                                    std::string_view key) {
    const json::Value* value = member(object, key);
    if (value == nullptr) return std::nullopt;
    if (value->kind != json::Kind::String && value->kind != json::Kind::Literal) {
        return std::nullopt;
    }
    if (json::is_blank(value->text)) return std::nullopt;
    return value->text;
}

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_object() {
    json::Value value;
    value.kind = json::Kind::Object;
    return value;
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

void write_inline_data(std::string& out, const ApiInlineData& data) {
    out += "{\"mimeType\":";
    append_quoted(out, data.mime_type);
    out += ",\"data\":";
    append_quoted(out, data.data);
    out += "}";
}

void write_executable_code(std::string& out, const ApiExecutableCode& code) {
    out += "{\"language\":";
    append_quoted(out, code.language);
    out += ",\"code\":";
    append_quoted(out, code.code);
    out += "}";
}

void write_code_execution_result(std::string& out, const ApiCodeExecutionResult& result) {
    out += "{\"outcome\":";
    append_quoted(out, result.outcome);
    out += ",\"output\":";
    append_quoted(out, result.output);
    out += "}";
}

void write_function_call(std::string& out, const GeminiFunctionCall& call) {
    out += "{";
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (call.id) {
        field("id");
        append_quoted(out, *call.id);
    }
    field("name");
    append_quoted(out, call.name);
    if (call.args) {
        field("args");
        out += json::to_kotlin_string(*call.args);
    }
    if (call.thought_signature) {
        field("thought_signature");
        append_quoted(out, *call.thought_signature);
    }
    out += "}";
}

void write_function_response(std::string& out, const GeminiFunctionResponse& response) {
    out += "{";
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (response.id) {
        field("id");
        append_quoted(out, *response.id);
    }
    field("name");
    append_quoted(out, response.name);
    field("response");
    out += json::to_kotlin_string(response.response);
    out += "}";
}

void write_part(std::string& out, const ApiRequestPart& part) {
    out += "{";
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (part.text) {
        field("text");
        append_quoted(out, *part.text);
    }
    if (part.inline_data) {
        field("inlineData");
        write_inline_data(out, *part.inline_data);
    }
    if (part.thought) {
        field("thought");
        append_quoted(out, *part.thought);
    }
    if (part.thought_signature) {
        field("thoughtSignature");
        append_quoted(out, *part.thought_signature);
    }
    if (part.executable_code) {
        field("executableCode");
        write_executable_code(out, *part.executable_code);
    }
    if (part.code_execution_result) {
        field("codeExecutionResult");
        write_code_execution_result(out, *part.code_execution_result);
    }
    if (part.function_call) {
        field("functionCall");
        write_function_call(out, *part.function_call);
    }
    if (part.function_response) {
        field("functionResponse");
        write_function_response(out, *part.function_response);
    }
    out += "}";
}

void write_content_body(std::string& out, const ApiRequestContent& content) {
    out += "{";
    bool first = true;
    if (content.role) {
        out += "\"role\":";
        append_quoted(out, *content.role);
        first = false;
    }
    if (!first) out.push_back(',');
    out += "\"parts\":[";
    for (std::size_t i = 0; i < content.parts.size(); ++i) {
        if (i > 0) out.push_back(',');
        write_part(out, content.parts[i]);
    }
    out += "]}";
}

void write_declaration(std::string& out, const GeminiFunctionDeclaration& declaration) {
    out += "{\"name\":";
    append_quoted(out, declaration.name);
    out += ",\"description\":";
    append_quoted(out, declaration.description);
    if (declaration.parameters) {
        out += ",\"parameters\":";
        out += json::to_kotlin_string(*declaration.parameters);
    }
    if (declaration.parameters_json_schema) {
        out += ",\"parameters_json_schema\":";
        out += json::to_kotlin_string(*declaration.parameters_json_schema);
    }
    out += "}";
}

void write_tool(std::string& out, const ApiTool& tool) {
    out += "{";
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (tool.code_execution) {
        field("code_execution");
        out += json::to_kotlin_string(*tool.code_execution);
    }
    if (tool.google_search) {
        field("google_search");
        out += json::to_kotlin_string(*tool.google_search);
    }
    if (tool.function_declarations) {
        field("function_declarations");
        out += "[";
        for (std::size_t i = 0; i < tool.function_declarations->size(); ++i) {
            if (i > 0) out.push_back(',');
            write_declaration(out, (*tool.function_declarations)[i]);
        }
        out += "]";
    }
    out += "}";
}

void write_thinking_config(std::string& out, const ApiThinkingConfig& config) {
    out += "{\"includeThoughts\":";
    out += config.include_thoughts ? "true" : "false";
    if (config.thinking_level) {
        out += ",\"thinkingLevel\":";
        append_quoted(out, *config.thinking_level);
    }
    if (config.thinking_budget) {
        out += ",\"thinkingBudget\":" + std::to_string(*config.thinking_budget);
    }
    out += "}";
}

void write_generation_config(std::string& out, const ApiGenerationConfig& config) {
    out += "{";
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (config.thinking_config) {
        field("thinkingConfig");
        write_thinking_config(out, *config.thinking_config);
    }
    if (config.temperature) {
        field("temperature");
        append_float(out, *config.temperature);
    }
    if (config.max_output_tokens) {
        field("maxOutputTokens");
        out += std::to_string(*config.max_output_tokens);
    }
    if (config.top_p) {
        field("topP");
        append_float(out, *config.top_p);
    }
    if (config.frequency_penalty) {
        field("frequencyPenalty");
        append_float(out, *config.frequency_penalty);
    }
    if (config.presence_penalty) {
        field("presencePenalty");
        append_float(out, *config.presence_penalty);
    }
    out += "}";
}

void write_request_body(std::string& out, const ApiGenerateContentRequest& request) {
    out += "{\"contents\":[";
    for (std::size_t i = 0; i < request.contents.size(); ++i) {
        if (i > 0) out.push_back(',');
        write_content_body(out, request.contents[i]);
    }
    out += "]";
    if (request.system_instruction) {
        out += ",\"systemInstruction\":";
        write_content_body(out, *request.system_instruction);
    }
    if (request.tools) {
        out += ",\"tools\":[";
        for (std::size_t i = 0; i < request.tools->size(); ++i) {
            if (i > 0) out.push_back(',');
            write_tool(out, (*request.tools)[i]);
        }
        out += "]";
    }
    if (request.tool_config) {
        out += ",\"toolConfig\":{\"includeServerSideToolInvocations\":";
        out += request.tool_config->include_server_side_tool_invocations ? "true" : "false";
        out += "}";
    }
    if (request.generation_config) {
        out += ",\"generationConfig\":";
        write_generation_config(out, *request.generation_config);
    }
    out += "}";
}

constexpr std::string_view kToolMsgPrefix = "tool_";
constexpr std::string_view kResultMsgPrefix = "result_";

}  // namespace

// ---------------------------------------------------------------------------------------------
// The request-side helpers from GeminiProvider.kt
// ---------------------------------------------------------------------------------------------

json::Value build_gemini_function_response(std::string_view result) {
    const json::ParseResult parsed = json::parse(result);
    if (parsed.value && parsed.value->kind == json::Kind::Object) return *parsed.value;
    json::Value wrapper = json_object();
    if (parsed.value) {
        wrapper.members.push_back({"result", *parsed.value});
    } else {
        wrapper.members.push_back({"result", json_string(std::string(result))});
    }
    return wrapper;
}

bool is_gemini_tool_round_compatible(const prep::ChatMessage& message,
                                     const std::string& target_model,
                                     const std::string& target_provider_name,
                                     bool signature_required) {
    struct CallView {
        std::optional<std::string> signature;
        std::optional<std::string> signature_provider;
    };
    std::vector<CallView> calls;
    if (message.segments) {
        for (const prep::MessageSegment& segment : *message.segments) {
            if (segment.type == "tool") {
                calls.push_back(CallView{segment.signature, segment.signature_provider});
            }
        }
    }
    if (calls.empty()) {
        if (message.tool_call) {
            // The legacy fallback copies the signature but NOT the signature provider.
            calls.push_back(CallView{message.tool_call->signature, std::nullopt});
        }
    }
    if (calls.empty()) return false;

    std::optional<ParsedModelId> stored_model;
    if (message.model_name) {
        const std::string trimmed = json::trim(*message.model_name);
        if (!trimmed.empty()) stored_model = parse_model_id(trimmed);
    }
    const std::string target = std::string(strip_models_prefix(json::trim(target_model)));
    for (const CallView& call : calls) {
        const bool blank = !call.signature || json::is_blank(*call.signature);
        if (signature_required && blank) return false;
        if (blank) continue;
        if (!stored_model) return false;
        std::string issuer = stored_model->provider_name;
        if (call.signature_provider) {
            const std::string candidate = json::trim(*call.signature_provider);
            if (!candidate.empty()) issuer = candidate;
        }
        if (!equals_ignore_ascii(issuer, target_provider_name)) return false;
        if (!equals_ignore_ascii(strip_models_prefix(stored_model->model_name), target)) {
            return false;
        }
    }
    return true;
}

std::vector<ApiRequestPart> gemini_model_message_parts(const prep::ChatMessage& message) {
    std::vector<const prep::MessageSegment*> segments;
    if (message.segments) {
        for (const prep::MessageSegment& segment : *message.segments) {
            segments.push_back(&segment);
        }
    }
    const auto has_code_execution = [&] {
        for (const prep::MessageSegment* segment : segments) {
            if (segment->type == "tool" && segment->tool_name == "code_execution") return true;
        }
        return false;
    };
    if (!has_code_execution()) {
        std::vector<ApiRequestPart> parts;
        if (!message.text.empty()) {
            ApiRequestPart part;
            part.text = message.text;
            parts.push_back(std::move(part));
        }
        return parts;
    }

    std::vector<ApiRequestPart> parts;
    bool included_answer_segment = false;
    for (const prep::MessageSegment* segment : segments) {
        if (segment->type == "answer" && !segment->content.empty()) {
            included_answer_segment = true;
            ApiRequestPart part;
            part.text = segment->content;
            parts.push_back(std::move(part));
        } else if (segment->type == "tool" && segment->tool_name == "code_execution") {
            // runCatching { Json.parseToJsonElement(toolArgs.orEmpty()) as? JsonObject }
            const json::ParseResult arguments =
                json::parse(segment->tool_args.value_or(""));
            const json::Value* arguments_object =
                arguments.value && arguments.value->kind == json::Kind::Object
                    ? &*arguments.value
                    : nullptr;
            const std::optional<std::string> code =
                arguments_object ? primitive_string_content(*arguments_object, "code")
                                 : std::nullopt;
            if (code) {
                ApiRequestPart part;
                ApiExecutableCode executable;
                executable.language =
                    arguments_object
                        ? primitive_string_content(*arguments_object, "language")
                                  .value_or("LANGUAGE_UNSPECIFIED")
                        : "LANGUAGE_UNSPECIFIED";
                executable.code = *code;
                part.executable_code = std::move(executable);
                parts.push_back(std::move(part));
            }
            const json::ParseResult result_parse =
                json::parse(segment->tool_result.value_or(""));
            const json::Value* result_object =
                result_parse.value && result_parse.value->kind == json::Kind::Object
                    ? &*result_parse.value
                    : nullptr;
            const std::optional<std::string> outcome =
                result_object ? primitive_string_content(*result_object, "outcome")
                              : std::nullopt;
            const json::Value* output_value =
                result_object ? member(*result_object, "output") : nullptr;
            const bool output_is_primitive =
                output_value != nullptr &&
                (output_value->kind == json::Kind::String ||
                 output_value->kind == json::Kind::Literal);
            if (outcome || output_is_primitive) {
                ApiRequestPart part;
                ApiCodeExecutionResult result;
                result.outcome = outcome.value_or("OUTCOME_UNSPECIFIED");
                result.output = output_is_primitive ? output_value->text : "";
                part.code_execution_result = std::move(result);
                parts.push_back(std::move(part));
            }
        }
    }
    if (!included_answer_segment && !message.text.empty()) {
        ApiRequestPart part;
        part.text = message.text;
        parts.insert(parts.begin(), std::move(part));
    }
    return parts;
}

std::vector<ApiRequestContent> coalesce_gemini_contents(
    const std::vector<ApiRequestContent>& contents) {
    std::vector<ApiRequestContent> result;
    for (const ApiRequestContent& content : contents) {
        if (!result.empty() && content.role && result.back().role == content.role) {
            result.back().parts.insert(result.back().parts.end(), content.parts.begin(),
                                       content.parts.end());
        } else {
            result.push_back(content);
        }
    }
    return result;
}

// ---------------------------------------------------------------------------------------------
// Validation gate — ApiGenerateContentRequest.requireValidWireFormat(modelName); the Kotlin
// extension hardcodes the "Gemini" provider string into its exception text.
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const ApiGenerateContentRequest& request,
                               const std::string& model_name) {
    std::vector<std::string> violations;

    if (json::is_blank(model_name)) violations.push_back("model is blank");
    if (request.contents.empty()) violations.push_back("contents is empty");

    if (request.system_instruction) {
        const ApiRequestContent& instruction = *request.system_instruction;
        if (instruction.role) violations.push_back("system_instruction must not carry a role");
        bool bad = instruction.parts.empty();
        for (const ApiRequestPart& part : instruction.parts) {
            if (!part.text || json::is_blank(*part.text)) bad = true;
            if (part.inline_data || part.function_call || part.function_response || part.thought ||
                part.thought_signature || part.executable_code || part.code_execution_result) {
                bad = true;
            }
        }
        if (bad) violations.push_back("system_instruction must contain only nonblank text");
    }

    if (request.tools) {
        std::vector<std::string> function_names;
        for (std::size_t tool_index = 0; tool_index < request.tools->size(); ++tool_index) {
            const ApiTool& tool = (*request.tools)[tool_index];
            const std::string location = "tools[" + std::to_string(tool_index) + "]";
            const int payload_count = (tool.code_execution ? 1 : 0) +
                                      (tool.google_search ? 1 : 0) +
                                      (tool.function_declarations ? 1 : 0);
            if (payload_count != 1) {
                violations.push_back(location + " must contain exactly one tool type");
            }
            if (!tool.function_declarations) continue;
            for (std::size_t function_index = 0;
                 function_index < tool.function_declarations->size(); ++function_index) {
                const GeminiFunctionDeclaration& declaration =
                    (*tool.function_declarations)[function_index];
                const std::string function_location =
                    location + ".functions[" + std::to_string(function_index) + "]";
                if (!wire_safe(declaration.name, 64)) {
                    violations.push_back(function_location + ".name is not wire-safe");
                }
                if (!add_unique(function_names, declaration.name)) {
                    violations.push_back("duplicate function declaration " + declaration.name);
                }
                if (declaration.parameters && declaration.parameters_json_schema) {
                    violations.push_back("function " + declaration.name +
                                         " declares both parameter schema forms");
                }
                const json::Value* schema =
                    declaration.parameters ? &*declaration.parameters
                                           : declaration.parameters_json_schema
                                                 ? &*declaration.parameters_json_schema
                                                 : nullptr;
                std::string rendered;
                if (schema != nullptr) {
                    const json::Value* type =
                        schema->kind == json::Kind::Object ? member(*schema, "type") : nullptr;
                    if (type != nullptr) rendered = json::to_kotlin_string(*type);
                    const std::size_t first = rendered.find_first_not_of('"');
                    if (first == std::string::npos) {
                        rendered.clear();
                    } else {
                        const std::size_t last = rendered.find_last_not_of('"');
                        rendered = rendered.substr(first, last - first + 1);
                    }
                }
                if (rendered != "object") {
                    violations.push_back("function " + declaration.name +
                                         " parameters are not an object schema");
                }
            }
        }
    }

    if (request.generation_config) {
        const ApiGenerationConfig& config = *request.generation_config;
        if (config.max_output_tokens && *config.max_output_tokens <= 0) {
            violations.push_back("maxOutputTokens must be positive");
        }
        if (config.top_p && !(*config.top_p >= 0.0f && *config.top_p <= 1.0f)) {
            violations.push_back("topP is outside 0..1");
        }
        if (config.thinking_config) {
            const ApiThinkingConfig& thinking = *config.thinking_config;
            if (thinking.thinking_budget && *thinking.thinking_budget < 0) {
                violations.push_back("thinkingBudget is negative");
            }
            if (thinking.thinking_level && json::is_blank(*thinking.thinking_level)) {
                violations.push_back("thinkingLevel is blank");
            }
        }
    }

    const bool requires_function_call_signature =
        contains_ignore_ascii(model_name, "gemini-3") ||
        contains_ignore_ascii(model_name, "gemini-3.5");

    std::vector<std::string> seen_call_ids;
    // Pending calls are an insertion-ordered id -> name map.
    std::vector<std::pair<std::string, std::string>> pending_calls;
    std::optional<std::string> previous_role;

    for (std::size_t content_index = 0; content_index < request.contents.size();
         ++content_index) {
        const ApiRequestContent& content = request.contents[content_index];
        const std::string location = "contents[" + std::to_string(content_index) + "]";

        if (!content.role || (*content.role != "user" && *content.role != "model")) {
            violations.push_back(location + " has invalid role " + content.role.value_or("null"));
        }
        if (previous_role && content.role && *previous_role == *content.role) {
            violations.push_back(location + " repeats role " + *content.role);
        }
        previous_role = content.role;
        if (content.parts.empty()) violations.push_back(location + " parts is empty");

        std::size_t leading_responses = 0;
        while (leading_responses < content.parts.size() &&
               content.parts[leading_responses].function_response) {
            ++leading_responses;
        }
        bool later_response = false;
        for (std::size_t j = leading_responses; j < content.parts.size(); ++j) {
            if (content.parts[j].function_response) {
                later_response = true;
                break;
            }
        }
        if (later_response) {
            violations.push_back(location + " has functionResponse after another part type");
        }

        const auto find_pending = [&](const std::string& id) -> const std::string* {
            for (const auto& [pending_id, pending_name] : pending_calls) {
                if (pending_id == id) return &pending_name;
            }
            return nullptr;
        };

        if (!pending_calls.empty()) {
            if (!content.role || *content.role != "user") {
                violations.push_back(location +
                                     " does not immediately answer pending function calls");
            }
            std::vector<std::string> returned_ids;
            std::vector<std::pair<std::string, std::string>> responses;
            for (std::size_t j = 0; j < leading_responses; ++j) {
                const GeminiFunctionResponse& response =
                    *content.parts[j].function_response;
                if (response.id) {
                    returned_ids.push_back(*response.id);
                    responses.push_back({*response.id, response.name});
                }
            }
            const bool same_size = returned_ids.size() == pending_calls.size();
            std::vector<std::string> pending_ids;
            for (const auto& [id, name] : pending_calls) {
                (void)name;
                pending_ids.push_back(id);
            }
            std::vector<std::string> unique_returned;
            for (const std::string& id : returned_ids) add_unique(unique_returned, id);
            const bool distinct = unique_returned.size() == returned_ids.size();
            const bool same_set = same_elements(returned_ids, pending_ids);
            if (!same_size || !distinct || !same_set) {
                violations.push_back(location +
                                     " does not answer every pending function call exactly once");
            }
            for (const auto& [id, name] : responses) {
                const std::string* expected = find_pending(id);
                if (expected != nullptr && name != *expected) {
                    violations.push_back(location + " response name does not match call " + id);
                }
            }
            pending_calls.clear();
        } else if (leading_responses > 0) {
            violations.push_back(location + " has orphan functionResponse parts");
        }

        for (std::size_t part_index = 0; part_index < content.parts.size(); ++part_index) {
            const ApiRequestPart& part = content.parts[part_index];
            const std::string part_location =
                location + ".parts[" + std::to_string(part_index) + "]";

            const int payload_count = (part.text ? 1 : 0) + (part.inline_data ? 1 : 0) +
                                      (part.thought ? 1 : 0) + (part.executable_code ? 1 : 0) +
                                      (part.code_execution_result ? 1 : 0) +
                                      (part.function_call ? 1 : 0) +
                                      (part.function_response ? 1 : 0);
            if (payload_count != 1) {
                violations.push_back(part_location + " must contain exactly one payload");
            }
            if (part.text && json::is_blank(*part.text)) {
                violations.push_back(part_location + " text is blank");
            }
            if (part.inline_data) {
                if (!content.role || *content.role != "user" ||
                    json::is_blank(part.inline_data->mime_type) ||
                    json::is_blank(part.inline_data->data)) {
                    violations.push_back(part_location +
                                         " is not a valid user inlineData part");
                }
            }
            if (part.executable_code) {
                if (!content.role || *content.role != "model" ||
                    json::is_blank(part.executable_code->language) ||
                    json::is_blank(part.executable_code->code)) {
                    violations.push_back(part_location + " is not valid model executableCode");
                }
            }
            if (part.code_execution_result) {
                if (!content.role || *content.role != "model" ||
                    json::is_blank(part.code_execution_result->outcome)) {
                    violations.push_back(part_location +
                                         " is not valid model codeExecutionResult");
                }
            }
            if (part.function_call) {
                const GeminiFunctionCall& call = *part.function_call;
                if (!content.role || *content.role != "model") {
                    violations.push_back(part_location + " functionCall is not model role");
                }
                if (!call.id || !wire_safe(*call.id, 128)) {
                    violations.push_back(part_location + " functionCall id is not wire-safe");
                }
                if (!wire_safe(call.name, 64)) {
                    violations.push_back(part_location + " functionCall name is not wire-safe");
                }
                if (requires_function_call_signature &&
                    (!part.thought_signature || json::is_blank(*part.thought_signature))) {
                    violations.push_back(part_location +
                                         " Gemini 3 functionCall has no thoughtSignature");
                }
                if (call.id && !json::is_blank(*call.id)) {
                    if (!add_unique(seen_call_ids, *call.id)) {
                        violations.push_back(part_location +
                                             " reuses functionCall id " + *call.id);
                    }
                    // LinkedHashMap.put: inserts or overwrites, returning the previous value.
                    bool present = false;
                    for (auto& [pending_id, pending_name] : pending_calls) {
                        if (pending_id == *call.id) {
                            pending_name = call.name;
                            present = true;
                            break;
                        }
                    }
                    if (present) {
                        violations.push_back(part_location +
                                             " duplicates functionCall id " + *call.id);
                    } else {
                        pending_calls.push_back({*call.id, call.name});
                    }
                }
            }
            if (part.function_response) {
                const GeminiFunctionResponse& response = *part.function_response;
                if (!content.role || *content.role != "user") {
                    violations.push_back(part_location +
                                         " functionResponse is not user role");
                }
                if (!response.id || !wire_safe(*response.id, 128)) {
                    violations.push_back(part_location +
                                         " functionResponse id is not wire-safe");
                }
                if (!wire_safe(response.name, 64)) {
                    violations.push_back(part_location +
                                         " functionResponse name is not wire-safe");
                }
            }
            if (part.thought_signature && !part.function_call) {
                violations.push_back(part_location +
                                     " thoughtSignature is not attached to a functionCall");
            }
        }
    }

    if (!pending_calls.empty()) violations.push_back("function calls are missing responses");
    if (request.contents.empty() || !request.contents.front().role ||
        *request.contents.front().role != "user") {
        violations.push_back("history does not start with user");
    }
    if (request.contents.empty() || !request.contents.back().role ||
        *request.contents.back().role != "user") {
        violations.push_back("history does not end with user input");
    }

    if (!violations.empty()) throw_request_format("Gemini", violations);
}

// ---------------------------------------------------------------------------------------------
// JSON writing — public content writers.
// ---------------------------------------------------------------------------------------------

std::string write_api_request_content(const ApiRequestContent& content) {
    std::string out;
    write_content_body(out, content);
    return out;
}

std::string write_api_request_content_list(const std::vector<ApiRequestContent>& contents) {
    std::string out;
    out += "[";
    for (std::size_t i = 0; i < contents.size(); ++i) {
        if (i > 0) out.push_back(',');
        write_content_body(out, contents[i]);
    }
    out += "]";
    return out;
}

// ---------------------------------------------------------------------------------------------
// The pipeline — buildApiContents + buildRequestBody and their surroundings.
// ---------------------------------------------------------------------------------------------

BuiltGeminiRequest build_gemini_request(const std::string& provider_name,
                                        const openai::ProviderConfig& config,
                                        const std::vector<prep::ChatMessage>& messages,
                                        const std::optional<std::string>& system_prompt) {
    const std::string clean_model = std::string(strip_models_prefix(config.model_id));
    const bool requires_function_call_signature =
        contains_ignore_ascii(clean_model, "gemini-3") ||
        contains_ignore_ascii(clean_model, "gemini-3.5");

    // Tool assembly: builtins first, function declarations last.
    std::vector<ApiTool> tools;
    if (config.code_execution_enabled) {
        ApiTool tool;
        tool.code_execution = json_object();
        tools.push_back(std::move(tool));
    }
    if (config.google_search_enabled) {
        ApiTool tool;
        tool.google_search = json_object();
        tools.push_back(std::move(tool));
    }
    if (config.tools && !config.tools->empty()) {
        ApiTool tool;
        std::vector<GeminiFunctionDeclaration> declarations;
        for (const openai::ToolDefinition& definition : *config.tools) {
            GeminiFunctionDeclaration declaration;
            declaration.name = definition.function.name;
            declaration.description = definition.function.description;
            if (definition.function.parameters.schema) {
                // An external (MCP) schema travels through parameters_json_schema.
                declaration.parameters_json_schema = *definition.function.parameters.schema;
            } else {
                declaration.parameters = openai::tool_schema_json_of(definition.function.parameters);
            }
            declarations.push_back(std::move(declaration));
        }
        tool.function_declarations = std::move(declarations);
        tools.push_back(std::move(tool));
    }

    // Thinking parameters come from the model's documented capability.
    const openai::ResolvedThinking resolved = openai::resolve_thinking(
        openai::capability_for_model(openai::ThinkingProviderFamily::Gemini, config.model_id),
        config.thinking_enabled, config.thinking_level, config.thinking_budget_enabled,
        config.thinking_budget_tokens);
    ApiThinkingConfig thinking_config;
    if (resolved.disabled()) {
        thinking_config.include_thoughts = false;
        if (resolved.capability.supports_thinking_budget) thinking_config.thinking_budget = 0;
    } else if (resolved.budget_tokens) {
        thinking_config.include_thoughts = true;
        thinking_config.thinking_budget = *resolved.budget_tokens;
    } else if (resolved.effort) {
        thinking_config.include_thoughts = true;
        thinking_config.thinking_level = upper_ascii(*resolved.effort);
    } else {
        thinking_config.include_thoughts = true;
    }

    const bool has_built_in_tools = config.code_execution_enabled || config.google_search_enabled;
    const bool has_function_declarations = !tools.empty() && tools.back().function_declarations;
    ApiToolConfig tool_config;
    tool_config.include_server_side_tool_invocations = true;
    const bool needs_tool_config = has_built_in_tools && has_function_declarations;

    ApiGenerationConfig generation_config;
    generation_config.thinking_config = thinking_config;
    generation_config.temperature = config.temperature;
    generation_config.max_output_tokens = config.max_tokens;
    generation_config.top_p = config.top_p;
    generation_config.frequency_penalty = config.frequency_penalty;
    generation_config.presence_penalty = config.presence_penalty;

    // Message walk over the adapted path.
    const std::vector<prep::ChatMessage> validated_path = prep::adapt_tool_rounds_for_provider(
        messages, provider_name, [&](const prep::ChatMessage& tool_message) {
            return is_gemini_tool_round_compatible(tool_message, clean_model, provider_name,
                                                   requires_function_call_signature);
        });

    openai::Base64FileRegistry base64_files;
    std::vector<ApiRequestContent> contents;
    for (const prep::ChatMessage& message : validated_path) {
        if (starts_with(message.id, kToolMsgPrefix)) {
            std::vector<ApiRequestPart> parts;
            std::vector<const prep::MessageSegment*> tool_segments;
            if (message.segments) {
                for (const prep::MessageSegment& segment : *message.segments) {
                    if (segment.type == "tool") tool_segments.push_back(&segment);
                }
            }
            if (!tool_segments.empty()) {
                for (const prep::MessageSegment* segment : tool_segments) {
                    ApiRequestPart part;
                    GeminiFunctionCall call;
                    call.id = segment->tool_call_id;
                    call.name = segment->tool_name.value_or("");
                    const json::ParseResult parsed =
                        json::parse(segment->tool_args.value_or("{}"));
                    if (parsed.value && parsed.value->kind == json::Kind::Object) {
                        call.args = *parsed.value;
                    } else {
                        call.args = json_object();
                    }
                    part.thought_signature = segment->signature;
                    part.function_call = std::move(call);
                    parts.push_back(std::move(part));
                }
                contents.push_back(ApiRequestContent{"model", std::move(parts)});
            } else if (message.tool_call) {
                ApiRequestPart part;
                GeminiFunctionCall call;
                const prep::ToolCallData& legacy = *message.tool_call;
                call.id = legacy.tool_call_id;
                call.name = legacy.tool_name;
                const json::ParseResult parsed = json::parse(legacy.arguments);
                if (parsed.value && parsed.value->kind == json::Kind::Object) {
                    call.args = *parsed.value;
                } else {
                    call.args = json_object();
                }
                part.thought_signature = legacy.signature;
                part.function_call = std::move(call);
                parts.push_back(std::move(part));
                contents.push_back(ApiRequestContent{"model", std::move(parts)});
            }
            continue;
        }
        if (starts_with(message.id, kResultMsgPrefix)) {
            std::vector<ApiRequestPart> parts;
            std::vector<const prep::MessageSegment*> tool_segments;
            if (message.segments) {
                for (const prep::MessageSegment& segment : *message.segments) {
                    if (segment.type == "tool") tool_segments.push_back(&segment);
                }
            }
            if (!tool_segments.empty()) {
                for (const prep::MessageSegment* segment : tool_segments) {
                    ApiRequestPart part;
                    GeminiFunctionResponse response;
                    response.id = segment->tool_call_id;
                    response.name = segment->tool_name.value_or("");
                    response.response =
                        build_gemini_function_response(segment->tool_result.value_or("{}"));
                    part.function_response = std::move(response);
                    parts.push_back(std::move(part));
                }
                contents.push_back(ApiRequestContent{"user", std::move(parts)});
            } else if (message.tool_call) {
                ApiRequestPart part;
                GeminiFunctionResponse response;
                const prep::ToolCallData& legacy = *message.tool_call;
                response.id = legacy.tool_call_id;
                response.name = legacy.tool_name;
                response.response = build_gemini_function_response(legacy.result);
                part.function_response = std::move(response);
                parts.push_back(std::move(part));
                contents.push_back(ApiRequestContent{"user", std::move(parts)});
            }
            continue;
        }

        // Normal message: model rows preserve code-execution history.
        std::vector<ApiRequestPart> parts;
        if (message.participant == prep::Participant::Model) {
            parts = gemini_model_message_parts(message);
        } else if (!message.text.empty()) {
            ApiRequestPart part;
            part.text = message.text;
            parts.push_back(std::move(part));
        }
        if (config.include_images && message.participant == prep::Participant::User) {
            for (const std::string& image_path : message.images) {
                const std::optional<std::string> placeholder =
                    base64_files.register_file(image_path);
                if (!placeholder) continue;
                ApiRequestPart part;
                part.inline_data = ApiInlineData{openai::image_mime_type(image_path), *placeholder};
                parts.push_back(std::move(part));
            }
        }
        if (parts.empty()) {
            ApiRequestPart part;
            part.text = "[Attachment unavailable]";
            parts.push_back(std::move(part));
        }
        contents.push_back(ApiRequestContent{
            message.participant == prep::Participant::User ? std::optional<std::string>{"user"}
                                                           : std::optional<std::string>{"model"},
            std::move(parts)});
    }
    contents = coalesce_gemini_contents(contents);

    ApiGenerateContentRequest request;
    request.contents = std::move(contents);
    if (system_prompt && !json::is_blank(*system_prompt)) {
        ApiRequestContent instruction;
        ApiRequestPart part;
        part.text = *system_prompt;
        instruction.parts.push_back(std::move(part));
        request.system_instruction = std::move(instruction);
    }
    if (!tools.empty()) request.tools = std::move(tools);
    if (needs_tool_config) request.tool_config = tool_config;
    request.generation_config = generation_config;

    // The provider order: wire gate, encode, base64 preparation, serialized-body gate.
    require_valid_wire_format(request, clean_model);

    std::string body_json;
    write_request_body(body_json, request);

    std::optional<openai::StreamingJsonBody> streaming_body = base64_files.prepare(body_json);

    openai::require_valid_serialized_request(provider_name, body_json, {}, {"contents"});

    BuiltGeminiRequest built;
    built.contents = std::move(request.contents);
    built.body_json = std::move(body_json);
    built.streaming_body = std::move(streaming_body);
    return built;
}

// ---------------------------------------------------------------------------------------------
// Serializable decode — Json { ignoreUnknownKeys = true }.decodeFromJsonElement(...) rules.
// ---------------------------------------------------------------------------------------------

ApiGenerateContentRequest decode_api_generate_content_request(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decoding("expected an object");

    ApiGenerateContentRequest request;

    const json::Value* contents = member(value, "contents");
    if (contents == nullptr) throw_missing_field("contents");
    if (is_null(contents)) throw_decoding("unexpected null");
    if (contents->kind != json::Kind::Array) throw_decoding("expected an array");
    for (const json::Value& entry : contents->elements) {
        if (entry.kind != json::Kind::Object) throw_decoding("expected an object");
        ApiRequestContent content;
        content.role = decode_optional_string(entry, "role");
        const json::Value* parts = member(entry, "parts");
        if (parts == nullptr) throw_missing_field("parts");
        if (is_null(parts)) throw_decoding("unexpected null");
        if (parts->kind != json::Kind::Array) throw_decoding("expected an array");
        for (const json::Value& part_entry : parts->elements) {
            if (part_entry.kind != json::Kind::Object) throw_decoding("expected an object");
            ApiRequestPart part;
            part.text = decode_optional_string(part_entry, "text");
            if (const json::Value* inline_data = member(part_entry, "inlineData");
                inline_data != nullptr && !is_null(inline_data)) {
                if (inline_data->kind != json::Kind::Object) throw_decoding("expected an object");
                ApiInlineData decoded;
                const json::Value* mime_type = member(*inline_data, "mimeType");
                if (mime_type == nullptr) throw_missing_field("mimeType");
                decoded.mime_type = decode_string(*mime_type);
                const json::Value* data = member(*inline_data, "data");
                if (data == nullptr) throw_missing_field("data");
                decoded.data = decode_string(*data);
                part.inline_data = std::move(decoded);
            }
            part.thought = decode_optional_string(part_entry, "thought");
            part.thought_signature = decode_optional_string(part_entry, "thoughtSignature");
            if (const json::Value* code = member(part_entry, "executableCode");
                code != nullptr && !is_null(code)) {
                if (code->kind != json::Kind::Object) throw_decoding("expected an object");
                ApiExecutableCode decoded;
                const json::Value* language = member(*code, "language");
                if (language == nullptr) throw_missing_field("language");
                decoded.language = decode_string(*language);
                const json::Value* body = member(*code, "code");
                if (body == nullptr) throw_missing_field("code");
                decoded.code = decode_string(*body);
                part.executable_code = std::move(decoded);
            }
            if (const json::Value* result = member(part_entry, "codeExecutionResult");
                result != nullptr && !is_null(result)) {
                if (result->kind != json::Kind::Object) throw_decoding("expected an object");
                ApiCodeExecutionResult decoded;
                const json::Value* outcome = member(*result, "outcome");
                if (outcome == nullptr) throw_missing_field("outcome");
                decoded.outcome = decode_string(*outcome);
                const json::Value* output = member(*result, "output");
                if (output == nullptr) throw_missing_field("output");
                decoded.output = decode_string(*output);
                part.code_execution_result = std::move(decoded);
            }
            if (const json::Value* call = member(part_entry, "functionCall");
                call != nullptr && !is_null(call)) {
                if (call->kind != json::Kind::Object) throw_decoding("expected an object");
                GeminiFunctionCall decoded;
                decoded.id = decode_optional_string(*call, "id");
                const json::Value* name = member(*call, "name");
                if (name == nullptr) throw_missing_field("name");
                decoded.name = decode_string(*name);
                if (const json::Value* args = member(*call, "args");
                    args != nullptr && !is_null(args)) {
                    if (args->kind != json::Kind::Object) throw_decoding("expected an object");
                    decoded.args = *args;
                }
                decoded.thought_signature = decode_optional_string(*call, "thought_signature");
                part.function_call = std::move(decoded);
            }
            if (const json::Value* response = member(part_entry, "functionResponse");
                response != nullptr && !is_null(response)) {
                if (response->kind != json::Kind::Object) throw_decoding("expected an object");
                GeminiFunctionResponse decoded;
                decoded.id = decode_optional_string(*response, "id");
                const json::Value* name = member(*response, "name");
                if (name == nullptr) throw_missing_field("name");
                decoded.name = decode_string(*name);
                const json::Value* body = member(*response, "response");
                if (body == nullptr) throw_missing_field("response");
                if (body->kind != json::Kind::Object) throw_decoding("expected an object");
                decoded.response = *body;
                part.function_response = std::move(decoded);
            }
            content.parts.push_back(std::move(part));
        }
        request.contents.push_back(std::move(content));
    }

    if (const json::Value* system_instruction = member(value, "systemInstruction");
        system_instruction != nullptr && !is_null(system_instruction)) {
        if (system_instruction->kind != json::Kind::Object) throw_decoding("expected an object");
        ApiRequestContent decoded;
        decoded.role = decode_optional_string(*system_instruction, "role");
        const json::Value* parts = member(*system_instruction, "parts");
        if (parts == nullptr) throw_missing_field("parts");
        if (parts->kind != json::Kind::Array) throw_decoding("expected an array");
        for (const json::Value& part_entry : parts->elements) {
            if (part_entry.kind != json::Kind::Object) throw_decoding("expected an object");
            ApiRequestPart part;
            part.text = decode_optional_string(part_entry, "text");
            part.thought = decode_optional_string(part_entry, "thought");
            part.thought_signature = decode_optional_string(part_entry, "thoughtSignature");
            decoded.parts.push_back(std::move(part));
        }
        request.system_instruction = std::move(decoded);
    }

    if (const json::Value* tools = member(value, "tools");
        tools != nullptr && !is_null(tools)) {
        if (tools->kind != json::Kind::Array) throw_decoding("expected an array");
        std::vector<ApiTool> decoded;
        for (const json::Value& entry : tools->elements) {
            if (entry.kind != json::Kind::Object) throw_decoding("expected an object");
            ApiTool tool;
            if (const json::Value* code_execution = member(entry, "code_execution");
                code_execution != nullptr && !is_null(code_execution)) {
                if (code_execution->kind != json::Kind::Object) {
                    throw_decoding("expected an object");
                }
                tool.code_execution = *code_execution;
            }
            if (const json::Value* google_search = member(entry, "google_search");
                google_search != nullptr && !is_null(google_search)) {
                if (google_search->kind != json::Kind::Object) throw_decoding("expected an object");
                tool.google_search = *google_search;
            }
            if (const json::Value* declarations = member(entry, "function_declarations");
                declarations != nullptr && !is_null(declarations)) {
                if (declarations->kind != json::Kind::Array) throw_decoding("expected an array");
                std::vector<GeminiFunctionDeclaration> decoded_declarations;
                for (const json::Value& declaration_entry : declarations->elements) {
                    if (declaration_entry.kind != json::Kind::Object) {
                        throw_decoding("expected an object");
                    }
                    GeminiFunctionDeclaration declaration;
                    const json::Value* name = member(declaration_entry, "name");
                    if (name == nullptr) throw_missing_field("name");
                    declaration.name = decode_string(*name);
                    const json::Value* description = member(declaration_entry, "description");
                    if (description == nullptr) throw_missing_field("description");
                    declaration.description = decode_string(*description);
                    if (const json::Value* parameters = member(declaration_entry, "parameters");
                        parameters != nullptr && !is_null(parameters)) {
                        if (parameters->kind != json::Kind::Object) {
                            throw_decoding("expected an object");
                        }
                        declaration.parameters = *parameters;
                    }
                    if (const json::Value* json_schema =
                            member(declaration_entry, "parameters_json_schema");
                        json_schema != nullptr && !is_null(json_schema)) {
                        if (json_schema->kind != json::Kind::Object) {
                            throw_decoding("expected an object");
                        }
                        declaration.parameters_json_schema = *json_schema;
                    }
                    decoded_declarations.push_back(std::move(declaration));
                }
                tool.function_declarations = std::move(decoded_declarations);
            }
            decoded.push_back(std::move(tool));
        }
        request.tools = std::move(decoded);
    }

    if (const json::Value* tool_config = member(value, "toolConfig");
        tool_config != nullptr && !is_null(tool_config)) {
        if (tool_config->kind != json::Kind::Object) throw_decoding("expected an object");
        ApiToolConfig decoded;
        if (const auto include = decode_optional_bool(*tool_config, "includeServerSideToolInvocations")) {
            decoded.include_server_side_tool_invocations = *include;
        }
        request.tool_config = decoded;
    }

    if (const json::Value* generation_config = member(value, "generationConfig");
        generation_config != nullptr && !is_null(generation_config)) {
        if (generation_config->kind != json::Kind::Object) throw_decoding("expected an object");
        ApiGenerationConfig decoded;
        if (const json::Value* thinking = member(*generation_config, "thinkingConfig");
            thinking != nullptr && !is_null(thinking)) {
            if (thinking->kind != json::Kind::Object) throw_decoding("expected an object");
            ApiThinkingConfig decoded_thinking;
            const json::Value* include = member(*thinking, "includeThoughts");
            if (include == nullptr) throw_missing_field("includeThoughts");
            decoded_thinking.include_thoughts = decode_bool_value(*include);
            decoded_thinking.thinking_level = decode_optional_string(*thinking, "thinkingLevel");
            decoded_thinking.thinking_budget = decode_optional_int(*thinking, "thinkingBudget");
            decoded.thinking_config = std::move(decoded_thinking);
        }
        decoded.temperature = decode_optional_float(*generation_config, "temperature");
        decoded.max_output_tokens = decode_optional_int(*generation_config, "maxOutputTokens");
        decoded.top_p = decode_optional_float(*generation_config, "topP");
        decoded.frequency_penalty = decode_optional_float(*generation_config, "frequencyPenalty");
        decoded.presence_penalty = decode_optional_float(*generation_config, "presencePenalty");
        request.generation_config = std::move(decoded);
    }

    return request;
}

}  // namespace agora::core::gemini
