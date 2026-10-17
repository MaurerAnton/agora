#include "agora/core/ollama.hpp"

#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdint>
#include <cstdlib>
#include <deque>
#include <optional>
#include <set>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/openai_request.hpp"
#include "agora/core/stream_normalization.hpp"

namespace agora::core::ollama {
namespace {

namespace json = agora::core::json;
namespace openai = agora::core::openai;
namespace prep = agora::core::prep;
namespace sn = agora::core::stream_normalization;

// ---------------------------------------------------------------------------------------------
// Small helpers
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

[[noreturn]] void throw_request_format(const std::string& provider,
                                       const std::vector<std::string>& violations) {
    std::vector<std::string> distinct;
    for (const std::string& violation : violations) {
        bool known = false;
        for (const std::string& entry : distinct) {
            if (entry == violation) {
                known = true;
                break;
            }
        }
        if (!known) distinct.push_back(violation);
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

[[noreturn]] void throw_decode(const std::string& what) {
    throw ProviderRequestError{"JsonDecodingException", what};
}

[[noreturn]] void throw_missing_field(const std::string& field) {
    throw ProviderRequestError{"MissingFieldException", "Field '" + field + "' is required"};
}

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

// ---------------------------------------------------------------------------------------------
// JSON writing — kotlinx with encodeDefaults=true, explicitNulls=false.
// ---------------------------------------------------------------------------------------------

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

json::Value json_string(std::string text) {
    json::Value value;
    value.kind = json::Kind::String;
    value.text = std::move(text);
    return value;
}

json::Value json_literal(std::string text) {
    json::Value value;
    value.kind = json::Kind::Literal;
    value.text = std::move(text);
    return value;
}

json::Value json_object() {
    json::Value value;
    value.kind = json::Kind::Object;
    return value;
}

/**
 * Writes the `options` object. Its members are number literals produced by
 * format_kotlin_float at build time; a non-finite spelling is rejected here, at encode time,
 * exactly like kotlinx rejects it when the request body is serialized.
 */
void write_options(std::string& out, const json::Value& options) {
    out.push_back('{');
    bool first = true;
    for (const json::Member& entry : options.members) {
        if (!first) out.push_back(',');
        first = false;
        append_quoted(out, entry.key);
        out.push_back(':');
        if (entry.value.kind == json::Kind::Literal &&
            (entry.value.text == "NaN" || entry.value.text == "Infinity" ||
             entry.value.text == "-Infinity")) {
            throw ProviderRequestError{
                "JsonEncodingException",
                "Unexpected special floating-point value " + entry.value.text +
                    ". By default, non-finite floating point values are prohibited because they "
                    "do not conform to JSON."};
        }
        if (entry.value.kind == json::Kind::Literal) {
            out += entry.value.text;
        } else {
            out += json::to_kotlin_string(entry.value);
        }
    }
    out.push_back('}');
}

void write_tool_call(std::string& out, const ToolCall& call) {
    out.push_back('{');
    bool first = true;
    const auto field = [&](const char* name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out += "\":";
    };
    if (call.index) {
        field("index");
        out += std::to_string(*call.index);
    }
    if (call.id) {
        field("id");
        append_quoted(out, *call.id);
    }
    if (call.type) {
        field("type");
        append_quoted(out, *call.type);
    }
    if (call.function) {
        field("function");
        out.push_back('{');
        bool function_first = true;
        const auto function_field = [&](const char* name) {
            if (!function_first) out.push_back(',');
            function_first = false;
            out.push_back('"');
            out += name;
            out += "\":";
        };
        if (call.function->name) {
            function_field("name");
            append_quoted(out, *call.function->name);
        }
        if (call.function->arguments) {
            function_field("arguments");
            out += json::to_kotlin_string(*call.function->arguments);
        }
        out.push_back('}');
    }
    out.push_back('}');
}

}  // namespace

void write_ollama_message_body(std::string& out, const OllamaMessage& message) {
    out += "{\"role\":";
    append_quoted(out, message.role);
    out += ",\"content\":";
    append_quoted(out, message.content);
    if (message.thinking) {
        out += ",\"thinking\":";
        append_quoted(out, *message.thinking);
    }
    if (message.images) {
        out += ",\"images\":[";
        for (std::size_t i = 0; i < message.images->size(); ++i) {
            if (i > 0) out.push_back(',');
            append_quoted(out, (*message.images)[i]);
        }
        out += "]";
    }
    if (message.tool_name) {
        out += ",\"tool_name\":";
        append_quoted(out, *message.tool_name);
    }
    if (message.tool_calls) {
        out += ",\"tool_calls\":[";
        for (std::size_t i = 0; i < message.tool_calls->size(); ++i) {
            if (i > 0) out.push_back(',');
            write_tool_call(out, (*message.tool_calls)[i]);
        }
        out += "]";
    }
    out += "}";
}

std::string write_ollama_message(const OllamaMessage& message) {
    std::string out;
    write_ollama_message_body(out, message);
    return out;
}

std::string write_ollama_message_list(const std::vector<OllamaMessage>& messages) {
    std::string out;
    out += "[";
    for (std::size_t i = 0; i < messages.size(); ++i) {
        if (i > 0) out.push_back(',');
        write_ollama_message_body(out, messages[i]);
    }
    out += "]";
    return out;
}

namespace {

void write_request_body(std::string& out, const OllamaChatRequest& request) {
    out += "{\"model\":";
    append_quoted(out, request.model);
    out += ",\"messages\":" + write_ollama_message_list(request.messages);
    // stream: encodeDefaults=true — always serialized.
    out += ",\"stream\":";
    out += request.stream ? "true" : "false";
    if (request.options) {
        out += ",\"options\":";
        write_options(out, *request.options);
    }
    if (request.tools) {
        out += ",\"tools\":[";
        for (std::size_t i = 0; i < request.tools->size(); ++i) {
            if (i > 0) out.push_back(',');
            openai::write_tool_definition(out, (*request.tools)[i]);
        }
        out += "]";
    }
    if (request.think) {
        out += ",\"think\":";
        out += json::to_kotlin_string(*request.think);
    }
    out += "}";
}

// ---------------------------------------------------------------------------------------------
// Message conversion — the provider's buildApiMessages (a local function in generateResponse).
// ---------------------------------------------------------------------------------------------

std::string last_thought_content(const std::optional<std::vector<prep::MessageSegment>>& segments) {
    if (!segments) return std::string();
    for (auto it = segments->rbegin(); it != segments->rend(); ++it) {
        if (it->type == "thought") return it->content;
    }
    return std::string();
}

}  // namespace

std::vector<OllamaMessage> build_api_messages(const openai::ProviderConfig& config,
                                              const std::vector<prep::ChatMessage>& messages,
                                              const std::optional<std::string>& system_prompt,
                                              openai::Base64FileRegistry& base64_files) {
    std::vector<OllamaMessage> api_messages;
    if (system_prompt && !json::is_blank(*system_prompt)) {
        OllamaMessage system;
        system.role = "system";
        system.content = *system_prompt;
        api_messages.push_back(std::move(system));
    }

    constexpr std::string_view kToolPrefix = "tool_";
    constexpr std::string_view kResultPrefix = "result_";

    for (const prep::ChatMessage& msg : messages) {
        // tool_ messages: assistant turn with tool_calls (thinking from the last thought segment)
        if (starts_with(msg.id, kToolPrefix)) {
            const std::string thinking_content = last_thought_content(msg.segments);
            const std::string thinking =
                thinking_content.empty() ? std::string() : thinking_content;

            std::vector<const prep::MessageSegment*> tool_segments;
            if (msg.segments) {
                for (const prep::MessageSegment& segment : *msg.segments) {
                    if (segment.type == "tool") tool_segments.push_back(&segment);
                }
            }
            if (!tool_segments.empty()) {
                std::vector<ToolCall> calls;
                for (const prep::MessageSegment* segment : tool_segments) {
                    ToolCall call;
                    call.id = segment->tool_call_id
                                  ? *segment->tool_call_id
                                  : openai::build_tool_call_id(
                                        segment->tool_name.value_or(""),
                                        segment->tool_args.value_or("{}"));
                    call.type = "function";
                    FunctionCall function;
                    function.name = segment->tool_name.value_or("");
                    const json::ParseResult parsed =
                        json::parse(segment->tool_args.value_or("{}"));
                    if (parsed.value && parsed.value->kind == json::Kind::Object) {
                        function.arguments = *parsed.value;
                    } else {
                        function.arguments = json_object();
                    }
                    call.function = std::move(function);
                    calls.push_back(std::move(call));
                }
                OllamaMessage out;
                out.role = "assistant";
                out.thinking = thinking.empty() ? std::nullopt
                                                : std::optional<std::string>{thinking};
                out.tool_calls = std::move(calls);
                api_messages.push_back(std::move(out));
            } else if (msg.tool_call) {
                const prep::ToolCallData& legacy = *msg.tool_call;
                ToolCall call;
                call.id = legacy.tool_call_id
                              ? *legacy.tool_call_id
                              : openai::build_tool_call_id(legacy.tool_name, legacy.arguments);
                call.type = "function";
                FunctionCall function;
                function.name = legacy.tool_name;
                const json::ParseResult parsed = json::parse(legacy.arguments);
                if (parsed.value && parsed.value->kind == json::Kind::Object) {
                    function.arguments = *parsed.value;
                } else {
                    function.arguments = json_object();
                }
                call.function = std::move(function);
                OllamaMessage out;
                out.role = "assistant";
                out.thinking = thinking.empty() ? std::nullopt
                                                : std::optional<std::string>{thinking};
                out.tool_calls = std::vector<ToolCall>{std::move(call)};
                api_messages.push_back(std::move(out));
            }
            continue;
        }

        // result_ messages carry the tool result(s)
        if (starts_with(msg.id, kResultPrefix)) {
            std::vector<const prep::MessageSegment*> tool_segments;
            if (msg.segments) {
                for (const prep::MessageSegment& segment : *msg.segments) {
                    if (segment.type == "tool") tool_segments.push_back(&segment);
                }
            }
            if (!tool_segments.empty()) {
                for (const prep::MessageSegment* segment : tool_segments) {
                    OllamaMessage out;
                    out.role = "tool";
                    out.content = segment->tool_result.value_or("");
                    out.tool_name = segment->tool_name;
                    api_messages.push_back(std::move(out));
                }
            } else if (msg.tool_call) {
                OllamaMessage out;
                out.role = "tool";
                out.content = msg.tool_call->result;
                out.tool_name = msg.tool_call->tool_name;
                api_messages.push_back(std::move(out));
            }
            continue;
        }

        // Normal message: text + registered images (user rows only, when enabled)
        std::optional<std::vector<std::string>> images;
        if (config.include_images && msg.participant == prep::Participant::User) {
            std::vector<std::string> registered;
            for (const std::string& path : msg.images) {
                const std::optional<std::string> placeholder = base64_files.register_file(path);
                if (placeholder) registered.push_back(*placeholder);
            }
            if (!registered.empty()) images = std::move(registered);
        }
        OllamaMessage out;
        out.role = msg.participant == prep::Participant::User ? "user" : "assistant";
        out.content = msg.text;
        out.images = images;
        api_messages.push_back(std::move(out));
    }
    return api_messages;
}

namespace {

// ---------------------------------------------------------------------------------------------
// options / think assembly (the provider's generateResponse preamble).
// ---------------------------------------------------------------------------------------------

std::optional<json::Value> build_options(const openai::ProviderConfig& config) {
    json::Value options = json_object();
    if (config.temperature) {
        options.members.push_back(
            {"temperature", json_literal(openai::format_kotlin_float(*config.temperature))});
    }
    if (config.top_p) {
        options.members.push_back(
            {"top_p", json_literal(openai::format_kotlin_float(*config.top_p))});
    }
    if (config.max_tokens) {
        options.members.push_back(
            {"num_predict", json_literal(std::to_string(*config.max_tokens))});
    }
    if (options.members.empty()) return std::nullopt;
    return options;
}

json::Value build_think(const openai::ProviderConfig& config) {
    const openai::ResolvedThinking resolved = openai::resolve_thinking(
        openai::capability_for_model(openai::ThinkingProviderFamily::Ollama, config.model_id),
        config.thinking_enabled, config.thinking_level, config.thinking_budget_enabled,
        config.thinking_budget_tokens);
    // think accepts a boolean or one of low/medium/high/max; the effort wins over the boolean.
    if (resolved.effort) return json_string(*resolved.effort);
    return json_literal(resolved.enabled ? "true" : "false");
}

}  // namespace

// ---------------------------------------------------------------------------------------------
// Validation gate — OllamaChatRequest.requireValidWireFormat (hardcoded "Ollama").
// ---------------------------------------------------------------------------------------------

void require_valid_wire_format(const OllamaChatRequest& request) {
    std::vector<std::string> violations;

    if (json::is_blank(request.model)) violations.push_back("model is blank");
    if (request.messages.empty()) violations.push_back("messages is empty");
    {
        const std::vector<std::string> tool_violations =
            openai::validate_tool_definitions(request.tools);
        violations.insert(violations.end(), tool_violations.begin(), tool_violations.end());
    }

    if (request.think) {
        const json::Value& think = *request.think;
        if (think.kind != json::Kind::String && think.kind != json::Kind::Literal) {
            violations.push_back("think must be a Boolean or effort string");
        } else {
            // booleanOrNull == null && (!isString || content !in {low, medium, high})
            const bool boolean_value =
                think.text == "true" || think.text == "false";
            const bool effort_value =
                think.kind == json::Kind::String &&
                (think.text == "low" || think.text == "medium" || think.text == "high");
            if (!boolean_value && !effort_value) {
                violations.push_back("think must be a Boolean or low/medium/high");
            }
        }
    }

    std::deque<std::string> pending_tool_names;
    bool saw_non_system = false;

    for (std::size_t index = 0; index < request.messages.size(); ++index) {
        const OllamaMessage& message = request.messages[index];
        const std::string location = "messages[" + std::to_string(index) + "]";
        if (message.role != "system" && message.role != "user" &&
            message.role != "assistant" && message.role != "tool") {
            violations.push_back(location + " has invalid role " + message.role);
        }
        const bool has_tool_calls =
            message.tool_calls && !message.tool_calls->empty();
        if (message.role == "system") {
            if (saw_non_system) violations.push_back(location + " system role is not at the beginning");
            if (json::is_blank(message.content)) violations.push_back(location + " system content is blank");
            if (has_tool_calls || message.tool_name) {
                violations.push_back(location + " system message carries tool fields");
            }
        } else if (message.role == "user") {
            saw_non_system = true;
            if (!pending_tool_names.empty()) {
                violations.push_back(location + " interrupts pending tool results");
            }
            const bool has_images = message.images && !message.images->empty();
            if (json::is_blank(message.content) && !has_images) {
                violations.push_back(location + " user content is empty");
            }
            if (has_tool_calls || message.tool_name) {
                violations.push_back(location + " user message carries tool fields");
            }
        } else if (message.role == "assistant") {
            saw_non_system = true;
            if (!pending_tool_names.empty()) {
                violations.push_back(location +
                                     " starts before prior tool results are complete");
                pending_tool_names.clear();
            }
            if (message.tool_name) violations.push_back(location + " assistant has tool_name");
            const std::vector<ToolCall> calls =
                message.tool_calls ? *message.tool_calls : std::vector<ToolCall>{};
            if (calls.empty() && json::is_blank(message.content) &&
                (!message.thinking || json::is_blank(*message.thinking))) {
                violations.push_back(location + " assistant content is empty");
            }
            for (std::size_t call_index = 0; call_index < calls.size(); ++call_index) {
                const ToolCall& call = calls[call_index];
                const std::string call_location =
                    location + ".tool_calls[" + std::to_string(call_index) + "]";
                const std::optional<std::string> name =
                    call.function ? call.function->name : std::nullopt;
                const json::Value* arguments = nullptr;
                if (call.function && call.function->arguments) {
                    arguments = &*call.function->arguments;
                }
                if (!call.type || *call.type != "function") {
                    violations.push_back(call_location + " type is not function");
                }
                if (!name || !wire_safe(*name, 64)) {
                    violations.push_back(call_location + " name is not wire-safe");
                }
                if (arguments == nullptr || arguments->kind != json::Kind::Object) {
                    violations.push_back(call_location + " arguments are not an object");
                }
                if (name && !json::is_blank(*name)) pending_tool_names.push_back(*name);
            }
        } else if (message.role == "tool") {
            saw_non_system = true;
            const std::optional<std::string> expected =
                pending_tool_names.empty() ? std::nullopt
                                           : std::optional<std::string>{pending_tool_names.front()};
            if (!pending_tool_names.empty()) pending_tool_names.pop_front();
            if (!message.tool_name || !wire_safe(*message.tool_name, 64)) {
                violations.push_back(location + " tool_name is not wire-safe");
            } else if (!expected || *expected != *message.tool_name) {
                violations.push_back(location + " does not match the pending tool call");
            }
            if (has_tool_calls) {
                violations.push_back(location + " tool result carries tool_calls");
            }
        }
    }

    if (!pending_tool_names.empty()) violations.push_back("tool calls are missing results");
    const std::optional<std::string> first_conversation_role = [&] {
        for (const OllamaMessage& message : request.messages) {
            if (message.role != "system") return std::optional<std::string>{message.role};
        }
        return std::optional<std::string>{};
    }();
    if (!first_conversation_role || *first_conversation_role != "user") {
        violations.push_back("history does not start with user");
    }
    const OllamaMessage* last =
        request.messages.empty() ? nullptr : &request.messages.back();
    const std::string last_role = last ? last->role : std::string();
    if (!last || (last_role != "user" && last_role != "tool")) {
        violations.push_back("history does not end in user/tool input");
    }

    if (!violations.empty()) throw_request_format("Ollama", violations);
}

// ---------------------------------------------------------------------------------------------
// The pipeline — buildRequestBody and its surroundings.
// ---------------------------------------------------------------------------------------------

BuiltOllamaRequest build_ollama_request(const openai::ProviderConfig& config,
                                        const std::vector<prep::ChatMessage>& messages,
                                        const std::optional<std::string>& system_prompt) {
    openai::Base64FileRegistry base64_files;

    OllamaChatRequest request;
    request.model = config.model_id;
    request.messages = build_api_messages(config, messages, system_prompt, base64_files);
    request.stream = true;
    request.options = build_options(config);
    request.tools = config.tools;
    request.think = build_think(config);

    // The provider order: wire gate, encode, base64 preparation, serialized-body gate.
    require_valid_wire_format(request);

    std::string body_json;
    write_request_body(body_json, request);

    std::optional<openai::StreamingJsonBody> streaming_body = base64_files.prepare(body_json);

    openai::require_valid_serialized_request("Ollama", body_json, {"model"}, {"messages"});

    BuiltOllamaRequest built;
    built.api_messages = std::move(request.messages);
    built.body_json = std::move(body_json);
    built.streaming_body = std::move(streaming_body);
    return built;
}

// ---------------------------------------------------------------------------------------------
// Serializable decode for the validate-mode harness.
// ---------------------------------------------------------------------------------------------

namespace {

std::string decode_string(const json::Value& value) {
    if (value.kind != json::Kind::String) throw_decode("expected a JSON string");
    return value.text;
}

std::optional<std::string> decode_optional_string(const json::Value& object,
                                                  std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_string(*value);
}

bool decode_bool_value(const json::Value& value) {
    if (value.kind != json::Kind::Literal || (value.text != "true" && value.text != "false")) {
        throw_decode("expected a JSON boolean");
    }
    return value.text == "true";
}

std::optional<bool> decode_optional_bool(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_bool_value(*value);
}

/** The number grammar kotlinx applies to numeric content (pinned by the golden traces). */
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

std::int32_t decode_int_like(const json::Value& value, const std::string& what) {
    if (value.kind != json::Kind::String && value.kind != json::Kind::Literal) {
        throw_decode("expected an integer for " + what);
    }
    const std::string_view text = value.text;
    if (!is_json_number_lexeme(text)) throw_decode("expected an Int for " + what);
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
        if (c < '0' || c > '9') throw_decode("expected an Int for " + what);
        accumulator = accumulator * 10 + static_cast<std::uint64_t>(c - '0');
        if (accumulator > limit) throw_decode("expected an Int for " + what);
    }
    if (negative) {
        if (accumulator == 2147483648ull) return INT32_MIN;
        return -static_cast<std::int32_t>(accumulator);
    }
    return static_cast<std::int32_t>(accumulator);
}

ToolCall decode_tool_call(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("tool call must be an object");
    ToolCall call;
    if (const json::Value* index = member(value, "index");
        index != nullptr && !is_null(index)) {
        call.index = decode_int_like(*index, "index");
    }
    call.id = decode_optional_string(value, "id");
    call.type = decode_optional_string(value, "type");
    if (const json::Value* function = member(value, "function");
        function != nullptr && !is_null(function)) {
        if (function->kind != json::Kind::Object) throw_decode("expected an object");
        FunctionCall decoded;
        decoded.name = decode_optional_string(*function, "name");
        if (const json::Value* arguments = member(*function, "arguments");
            arguments != nullptr && !is_null(arguments)) {
            decoded.arguments = *arguments;
        }
        call.function = std::move(decoded);
    }
    return call;
}

OllamaMessage decode_message(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("message must be an object");
    OllamaMessage message;
    const json::Value* role = member(value, "role");
    if (role == nullptr) throw_missing_field("role");
    message.role = decode_string(*role);
    if (const auto content = decode_optional_string(value, "content")) message.content = *content;
    message.thinking = decode_optional_string(value, "thinking");
    if (const json::Value* images = member(value, "images");
        images != nullptr && !is_null(images)) {
        if (images->kind != json::Kind::Array) throw_decode("expected an array");
        std::vector<std::string> decoded;
        for (const json::Value& entry : images->elements) {
            decoded.push_back(decode_string(entry));
        }
        message.images = std::move(decoded);
    }
    message.tool_name = decode_optional_string(value, "tool_name");
    if (const json::Value* calls = member(value, "tool_calls");
        calls != nullptr && !is_null(calls)) {
        if (calls->kind != json::Kind::Array) throw_decode("expected an array");
        std::vector<ToolCall> decoded;
        for (const json::Value& entry : calls->elements) {
            decoded.push_back(decode_tool_call(entry));
        }
        message.tool_calls = std::move(decoded);
    }
    return message;
}

}  // namespace

OllamaChatRequest decode_ollama_chat_request(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("expected an object");

    OllamaChatRequest request;
    const json::Value* model = member(value, "model");
    if (model == nullptr) throw_missing_field("model");
    request.model = decode_string(*model);

    const json::Value* messages = member(value, "messages");
    if (messages == nullptr) throw_missing_field("messages");
    if (is_null(messages)) throw_decode("unexpected null");
    if (messages->kind != json::Kind::Array) throw_decode("expected an array");
    for (const json::Value& entry : messages->elements) {
        request.messages.push_back(decode_message(entry));
    }

    if (const auto stream = decode_optional_bool(value, "stream")) request.stream = *stream;
    if (const json::Value* options = member(value, "options");
        options != nullptr && !is_null(options)) {
        if (options->kind != json::Kind::Object) throw_decode("expected an object");
        request.options = *options;
    }
    if (const json::Value* tools = member(value, "tools");
        tools != nullptr && !is_null(tools)) {
        if (tools->kind != json::Kind::Array) throw_decode("expected an array");
        std::vector<openai::ToolDefinition> decoded;
        for (const json::Value& entry : tools->elements) {
            decoded.push_back(openai::decode_tool_definition(entry));
        }
        request.tools = std::move(decoded);
    }
    if (const json::Value* think = member(value, "think");
        think != nullptr && !is_null(think)) {
        request.think = *think;  // JsonElement — any kind; the validator decides
    }
    return request;
}

// ---------------------------------------------------------------------------------------------
// Stream side — the NDJSON loop.
// ---------------------------------------------------------------------------------------------

std::optional<OllamaStreamResponse> decode_ollama_stream_response(std::string_view json_text,
                                                                  DecodeFailure* failure) {
    try {
        const json::ParseResult parsed = json::parse(json_text);
        if (!parsed.value || parsed.value->kind != json::Kind::Object) {
            throw_decode("expected an object");
        }
        const json::Value& value = *parsed.value;
        OllamaStreamResponse response;
        response.model = decode_optional_string(value, "model");
        if (const json::Value* message = member(value, "message");
            message != nullptr && !is_null(message)) {
            response.message = decode_message(*message);
        }
        if (const auto done = decode_optional_bool(value, "done")) response.done = *done;
        response.done_reason = decode_optional_string(value, "done_reason");
        response.error = decode_optional_string(value, "error");
        if (const json::Value* prompt = member(value, "prompt_eval_count");
            prompt != nullptr && !is_null(prompt)) {
            response.prompt_eval_count = decode_int_like(*prompt, "prompt_eval_count");
        }
        if (const json::Value* eval_count = member(value, "eval_count");
            eval_count != nullptr && !is_null(eval_count)) {
            response.eval_count = decode_int_like(*eval_count, "eval_count");
        }
        return response;
    } catch (const ProviderRequestError& error) {
        if (failure != nullptr) failure->message = error.message;
        return std::nullopt;
    } catch (const std::exception& error) {
        if (failure != nullptr) failure->message = error.what();
        return std::nullopt;
    }
}

TokenUsage to_token_usage(const OllamaStreamResponse& response) {
    const auto coerce_zero = [](const std::optional<std::int32_t>& value) {
        if (!value) return std::optional<std::int32_t>{};
        return std::optional<std::int32_t>{std::max(*value, static_cast<std::int32_t>(0))};
    };
    const std::optional<std::int32_t> input = coerce_zero(response.prompt_eval_count);
    const std::optional<std::int32_t> output = coerce_zero(response.eval_count);
    std::int32_t total = 0;
    if (input && output) {
        total = add_token_counts(*input, *output);
    } else if (input) {
        total = *input;
    } else {
        total = output.value_or(0);
    }
    TokenUsage usage;
    usage.total_token_count = total;
    usage.input_token_count = input;
    // Ollama reports prompt evaluation, but not a cache hit/miss split — and no reasoning split.
    usage.output_token_count = output;
    return usage;
}

sn::StreamTermination ollama_stream_termination(bool saw_done,
                                                const std::optional<std::string>& done_reason,
                                                bool produced_content, bool tool_call_in_flight,
                                                const std::optional<sn::GenerationError>& stream_error,
                                                bool timed_out) {
    sn::StreamTermination termination;
    termination.saw_terminal_marker = saw_done;
    if (done_reason) {
        const std::string trimmed = json::trim(*done_reason);
        if (!trimmed.empty()) termination.stop_reason = lower_ascii(trimmed);
    }
    termination.produced_content = produced_content;
    termination.tool_call_in_flight = tool_call_in_flight;
    termination.stream_error = stream_error;
    termination.timed_out = timed_out;
    return termination;
}

bool carries_model_output(const StreamEvent& event) {
    struct Visitor {
        bool operator()(const sn::TextChunk& chunk) const { return !chunk.text.empty(); }
        bool operator()(const sn::ThoughtChunk& chunk) const {
            return !chunk.thought.empty() || chunk.signature.has_value();
        }
        bool operator()(const sn::ToolCallUpdate&) const { return true; }
        bool operator()(const sn::ToolCallRequest&) const { return true; }
        bool operator()(const sn::ToolCallsRequest&) const { return true; }
        bool operator()(const UsageUpdate&) const { return false; }
    };
    return std::visit(Visitor{}, event);
}

OllamaRouter::OllamaRouter(sn::UuidFn uuid) : uuid_(std::move(uuid)) {}

void OllamaRouter::capture_parse_error(std::string_view raw_line, std::string cause) {
    stream_error = sn::ErrorSseParse{json::take_utf16_units(raw_line, 512), std::move(cause)};
}

std::vector<StreamEvent> OllamaRouter::route(const OllamaStreamResponse& response) {
    std::vector<StreamEvent> out;

    if (response.done_reason && !json::is_blank(*response.done_reason)) {
        done_reason = *response.done_reason;
    }
    if (response.error && !json::is_blank(*response.error)) {
        stream_error = sn::ErrorApi{std::nullopt, "ollama_stream_error", *response.error};
    }

    if (response.message) {
        const OllamaMessage& message = *response.message;

        // 1. Explicit thinking field (Ollama 0.5.4+)
        if (message.thinking && !message.thinking->empty()) {
            out.push_back(sn::ThoughtChunk{*message.thinking, std::nullopt, std::nullopt});
        }

        // 2. Ollama tool calls are complete snapshots in one NDJSON message. The whole batch is
        //    kept only when every call has a wire-safe name/id AND a JSON-object argument payload;
        //    duplicates and damaged calls become the malformed stand-in.
        if (message.tool_calls && !message.tool_calls->empty()) {
            struct ParsedCall {
                sn::ToolCallUpdate update;
                sn::ToolCallRequest call;
                bool valid = false;
            };
            std::vector<ParsedCall> parsed;
            for (const ToolCall& tool_call : *message.tool_calls) {
                ParsedCall entry;
                const std::string stream_key = "call_stream_" + uuid_();
                std::string id;
                if (tool_call.id && !json::is_blank(*tool_call.id)) {
                    id = *tool_call.id;
                } else {
                    id = "call_" + uuid_();
                }
                const std::string name =
                    tool_call.function && tool_call.function->name
                        ? *tool_call.function->name
                        : std::string();
                std::string args;
                if (tool_call.function && tool_call.function->arguments) {
                    const json::Value& value = *tool_call.function->arguments;
                    if (value.kind == json::Kind::String || value.kind == json::Kind::Literal) {
                        args = value.text;
                    } else {
                        args = json::to_kotlin_string(value);
                    }
                }
                if (json::is_blank(args)) args = "{}";
                bool valid = false;
                if (wire_safe(name, 64) && wire_safe(id, 128)) {
                    const json::ParseResult parsed_args = json::parse(args);
                    valid = parsed_args.value && parsed_args.value->kind == json::Kind::Object;
                }
                entry.update = sn::ToolCallUpdate{stream_key, id, name, args, std::nullopt};
                entry.call = sn::ToolCallRequest{id, name, args, std::nullopt, stream_key, 0};
                entry.valid = valid;
                parsed.push_back(std::move(entry));
            }

            std::vector<std::string> seen_ids;
            std::vector<sn::ToolCallRequest> calls;
            for (ParsedCall& entry : parsed) {
                bool keep = entry.valid;
                if (keep &&
                    std::find(seen_ids.begin(), seen_ids.end(), entry.call.id) != seen_ids.end()) {
                    keep = false;
                }
                if (keep) {
                    seen_ids.push_back(entry.call.id);
                    calls.push_back(std::move(entry.call));
                } else {
                    calls.push_back(sn::malformed_tool_call_request(
                        "Ollama returned incomplete or duplicate tool metadata",
                        entry.update.name, entry.update.arguments, entry.update.stream_key,
                        std::nullopt, uuid_));
                }
            }
            for (const ParsedCall& entry : parsed) out.push_back(entry.update);
            if (calls.size() == 1) {
                out.push_back(calls.front());
            } else {
                sn::ToolCallsRequest batch;
                batch.calls = std::move(calls);
                out.push_back(std::move(batch));
            }
        }

        // 3. Inline content
        if (!message.content.empty()) out.push_back(sn::TextChunk{message.content});
    }

    if (response.done) {
        saw_done = true;
        out.push_back(UsageUpdate{to_token_usage(response)});
    }
    return out;
}

}  // namespace agora::core::ollama