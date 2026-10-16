// kotlinx-exact JSON serialization for the OpenAI chat wire models.
//
// Field order, null-omission (explicitNulls=false) and default inclusion (encodeDefaults=true)
// mirror ProviderRequestModels.kt; string escaping reuses the pinned StringOps escaping in
// json::escape_to, and Float formatting reproduces the JVM's shortest round-trip form as pinned
// by core/tools/golden/openai-request golden traces (chat_float_*).
#include <algorithm>
#include <charconv>
#include <cmath>
#include <cstdio>
#include <cstdlib>
#include <cstring>
#include <limits>
#include <system_error>

#include "agora/core/openai_request.hpp"

namespace agora::core::openai {
namespace {

namespace json = agora::core::json;

[[noreturn]] void throw_decode(const std::string& what) {
    throw ProviderRequestError{"JsonDecodingException", what};
}

/**
 * Decimal text -> float, correctly rounded, whole-string match like String.toFloat().
 * std::from_chars for floats is deleted in the NDK's libc++, so it cannot be used on Android;
 * strtof is correctly rounded on both bionic and glibc. Native code keeps the C locale.
 */
bool parse_float(std::string_view text, float& out) {
    if (text.empty()) return false;
    const std::string copy(text);
    const char* const begin = copy.c_str();
    char* end = nullptr;
    out = std::strtof(begin, &end);
    if (end != begin + copy.size()) return false;
    // Strict JSON never carries non-finite literals; the kotlinx decoder rejects them too.
    return std::isfinite(out);
}

void append_quoted(std::string& out, std::string_view text) {
    out.push_back('"');
    json::escape_to(out, text);
    out.push_back('"');
}

/** kotlinx Json encoder for a Float: non-finite values are rejected before formatting. */
void append_float(std::string& out, float value) {
    if (!std::isfinite(value)) {
        throw ProviderRequestError{"JsonEncodingException",
                                   "Unexpected special floating-point value " +
                                       format_kotlin_float(value) +
                                       ". By default, non-finite floating point values are "
                                       "prohibited because they do not conform to JSON."};
    }
    out += format_kotlin_float(value);
}

/** Tracks the "write ',' before the next member" rule of a kotlinx JSON object/array. */
struct MemberList {
    std::string out{"{"};
    bool first = true;

    void begin_member(std::string_view name) {
        if (!first) out.push_back(',');
        first = false;
        out.push_back('"');
        out += name;
        out.push_back('"');
        out.push_back(':');
    }

    void begin_element() {
        if (!first) out.push_back(',');
        first = false;
    }

    std::string finish() {
        out.push_back('}');
        return std::move(out);
    }
};

void write_content_parts(std::string& out, const std::vector<OpenAiContentPart>& parts);

void write_tool_calls(std::string& out, const std::vector<OpenAiRequestToolCall>& calls);

void write_tool_property(std::string& out, const ToolProperty& property) {
    MemberList member;
    member.begin_member("type");
    append_quoted(member.out, property.type);
    member.begin_member("description");
    append_quoted(member.out, property.description);
    if (property.items) {
        member.begin_member("items");
        write_tool_property(member.out, *property.items);
    }
    if (property.properties) {
        member.begin_member("properties");
        MemberList nested;
        for (const auto& [name, child] : *property.properties) {
            nested.begin_member(name);
            write_tool_property(nested.out, child);
        }
        nested.out.push_back('}');
        member.out += nested.out;
    }
    if (property.required) {
        member.begin_member("required");
        member.out.push_back('[');
        bool first = true;
        for (const std::string& name : *property.required) {
            if (!first) member.out.push_back(',');
            first = false;
            append_quoted(member.out, name);
        }
        member.out.push_back(']');
    }
    out += member.finish();
}

/** ToolParametersSerializer: the external schema verbatim (JsonElement re-serialized), or the
 *  typed trio {type, properties, required} — all three always written (encodeDefaults). */
void write_tool_parameters(std::string& out, const ToolParameters& parameters) {
    if (parameters.schema) {
        out += json::to_kotlin_string(*parameters.schema);
        return;
    }
    MemberList member;
    member.begin_member("type");
    append_quoted(member.out, parameters.type);
    member.begin_member("properties");
    member.out.push_back('{');
    bool first = true;
    for (const auto& [name, property] : parameters.properties) {
        if (!first) member.out.push_back(',');
        first = false;
        append_quoted(member.out, name);
        member.out.push_back(':');
        write_tool_property(member.out, property);
    }
    member.out.push_back('}');
    member.begin_member("required");
    member.out.push_back('[');
    first = true;
    for (const std::string& name : parameters.required) {
        if (!first) member.out.push_back(',');
        first = false;
        append_quoted(member.out, name);
    }
    member.out.push_back(']');
    out += member.finish();
}

void write_tool_definition(std::string& out, const ToolDefinition& tool) {
    MemberList member;
    member.begin_member("type");
    append_quoted(member.out, tool.type);
    member.begin_member("function");
    MemberList function;
    function.begin_member("name");
    append_quoted(function.out, tool.function.name);
    function.begin_member("description");
    append_quoted(function.out, tool.function.description);
    function.begin_member("parameters");
    write_tool_parameters(function.out, tool.function.parameters);
    function.out.push_back('}');
    member.out += function.out;
    out += member.finish();
}

void write_content_parts(std::string& out, const std::vector<OpenAiContentPart>& parts) {
    out.push_back('[');
    bool first_part = true;
    for (const OpenAiContentPart& part : parts) {
        if (!first_part) out.push_back(',');
        first_part = false;
        MemberList member;
        member.begin_member("type");
        append_quoted(member.out, part.type);
        if (part.text) {
            member.begin_member("text");
            append_quoted(member.out, *part.text);
        }
        if (part.image_url) {
            member.begin_member("image_url");
            MemberList image;
            image.begin_member("url");
            append_quoted(image.out, part.image_url->url);
            image.out.push_back('}');
            member.out += image.out;
        }
        member.out.push_back('}');
        out += member.out;
    }
    out.push_back(']');
}

void write_tool_calls(std::string& out, const std::vector<OpenAiRequestToolCall>& calls) {
    out.push_back('[');
    bool first_call = true;
    for (const OpenAiRequestToolCall& call : calls) {
        if (!first_call) out.push_back(',');
        first_call = false;
        MemberList member;
        member.begin_member("id");
        append_quoted(member.out, call.id);
        member.begin_member("type");
        append_quoted(member.out, call.type);
        member.begin_member("function");
        MemberList function;
        function.begin_member("name");
        append_quoted(function.out, call.function.name);
        function.begin_member("arguments");
        append_quoted(function.out, call.function.arguments);
        function.out.push_back('}');
        member.out += function.out;
        member.out.push_back('}');
        out += member.out;
    }
    out.push_back(']');
}

void write_int(std::string& out, std::int32_t value) {
    char buffer[16];
    const auto result = std::to_chars(buffer, buffer + sizeof(buffer), value);
    out.append(buffer, result.ptr);
}

// -- decode helpers -----------------------------------------------------------------------------

/** JsonObject lookup: the last duplicate key wins, like kotlinx LinkedHashMap decoding. */
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
    if (value.kind != json::Kind::Literal) throw_decode("expected a JSON boolean");
    if (value.text == "true") return true;
    if (value.text == "false") return false;
    throw_decode("expected a JSON boolean");
}

std::optional<bool> decode_optional_bool(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_bool_value(*value);
}

std::int32_t decode_int_value(const json::Value& value) {
    if (value.kind != json::Kind::Literal) throw_decode("expected an integer");
    if (value.text.find_first_of(".eE") != std::string::npos) throw_decode("expected an integer");
    std::int32_t parsed = 0;
    const char* begin = value.text.data();
    const char* end = begin + value.text.size();
    const auto result = std::from_chars(begin, end, parsed);
    if (result.ec != std::errc{} || result.ptr != end) throw_decode("expected an Int");
    return parsed;
}

std::optional<std::int32_t> decode_optional_int(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_int_value(*value);
}

float decode_float_value(const json::Value& value) {
    if (value.kind != json::Kind::Literal) throw_decode("expected a JSON number");
    float parsed = 0.0f;
    if (!parse_float(value.text, parsed)) throw_decode("expected a Float");
    return parsed;
}

std::optional<float> decode_optional_float(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) return std::nullopt;
    return decode_float_value(*value);
}

const json::Value& decode_object_field(const json::Value& object, std::string_view name) {
    const json::Value* value = member(object, name);
    if (value == nullptr || is_null(value)) throw_decode("field '" + std::string(name) +
                                                         "' is required");
    if (value->kind != json::Kind::Object) throw_decode("field '" + std::string(name) +
                                                        "' must be an object");
    return *value;
}

OpenAiContentPart decode_content_part(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("content part must be an object");
    OpenAiContentPart part;
    const json::Value* type = member(value, "type");
    if (type == nullptr) throw_decode("content part requires type");
    part.type = decode_string(*type);
    part.text = decode_optional_string(value, "text");
    const json::Value* image_url = member(value, "image_url");
    if (image_url != nullptr && !is_null(image_url)) {
        if (image_url->kind != json::Kind::Object) throw_decode("image_url must be an object");
        const json::Value* url = member(*image_url, "url");
        if (url == nullptr) throw_decode("image_url requires url");
        part.image_url = OpenAiImageUrl{decode_string(*url)};
    }
    return part;
}

OpenAiRequestToolCall decode_tool_call(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("tool call must be an object");
    OpenAiRequestToolCall call;
    const json::Value* id = member(value, "id");
    if (id == nullptr) throw_decode("tool call requires id");
    call.id = decode_string(*id);
    const json::Value* type = member(value, "type");
    if (type != nullptr && !is_null(type)) call.type = decode_string(*type);
    const json::Value& function = decode_object_field(value, "function");
    call.function.name = decode_string([&] {
        const json::Value* name = member(function, "name");
        if (name == nullptr) throw_decode("tool call function requires name");
        return *name;
    }());
    call.function.arguments = decode_string([&] {
        const json::Value* arguments = member(function, "arguments");
        if (arguments == nullptr) throw_decode("tool call function requires arguments");
        return *arguments;
    }());
    return call;
}

OpenAiMessage decode_message(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("message must be an object");
    OpenAiMessage message;
    const json::Value* role = member(value, "role");
    if (role == nullptr) throw_decode("message requires role");
    message.role = decode_string(*role);

    const json::Value* content = member(value, "content");
    if (content != nullptr && !is_null(content)) {
        if (content->kind != json::Kind::Array) throw_decode("content must be an array");
        std::vector<OpenAiContentPart> parts;
        parts.reserve(content->elements.size());
        for (const json::Value& part : content->elements) parts.push_back(decode_content_part(part));
        message.content = std::move(parts);
    }

    const json::Value* tool_calls = member(value, "tool_calls");
    if (tool_calls != nullptr && !is_null(tool_calls)) {
        if (tool_calls->kind != json::Kind::Array) throw_decode("tool_calls must be an array");
        std::vector<OpenAiRequestToolCall> calls;
        calls.reserve(tool_calls->elements.size());
        for (const json::Value& call : tool_calls->elements) calls.push_back(decode_tool_call(call));
        message.tool_calls = std::move(calls);
    }

    message.tool_call_id = decode_optional_string(value, "tool_call_id");
    message.reasoning_content = decode_optional_string(value, "reasoning_content");
    return message;
}

ToolProperty decode_tool_property(const json::Value& value, const std::string& name) {
    if (value.kind != json::Kind::Object) throw_decode("property '" + name + "' must be an object");
    ToolProperty property;
    const json::Value* type = member(value, "type");
    if (type == nullptr) throw_decode("property '" + name + "' requires type");
    property.type = decode_string(*type);
    const json::Value* description = member(value, "description");
    if (description == nullptr) throw_decode("property '" + name + "' requires description");
    property.description = decode_string(*description);

    const json::Value* items = member(value, "items");
    if (items != nullptr && !is_null(items)) {
        property.items = std::make_shared<ToolProperty>(decode_tool_property(*items, name + "[]"));
    }

    const json::Value* properties = member(value, "properties");
    if (properties != nullptr && !is_null(properties)) {
        if (properties->kind != json::Kind::Object) throw_decode("properties must be an object");
        std::vector<std::pair<std::string, ToolProperty>> nested;
        nested.reserve(properties->members.size());
        for (const json::Member& entry : properties->members) {
            nested.emplace_back(entry.key, decode_tool_property(entry.value, entry.key));
        }
        property.properties = std::move(nested);
    }

    const json::Value* required = member(value, "required");
    if (required != nullptr && !is_null(required)) {
        if (required->kind != json::Kind::Array) throw_decode("required must be an array");
        std::vector<std::string> names;
        names.reserve(required->elements.size());
        for (const json::Value& entry : required->elements) names.push_back(decode_string(entry));
        property.required = std::move(names);
    }
    return property;
}

ToolParameters decode_tool_parameters(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("parameters must be an object");
    ToolParameters parameters;
    // ToolParametersSerializer.deserialize reads the typed fields only; `schema` never round-trips.
    const json::Value* type = member(value, "type");
    if (type != nullptr && !is_null(type)) parameters.type = decode_string(*type);
    const json::Value* properties = member(value, "properties");
    if (properties == nullptr || is_null(properties) || properties->kind != json::Kind::Object) {
        throw_decode("parameters require properties");
    }
    for (const json::Member& entry : properties->members) {
        parameters.properties.emplace_back(entry.key, decode_tool_property(entry.value, entry.key));
    }
    const json::Value* required = member(value, "required");
    if (required != nullptr && !is_null(required)) {
        if (required->kind != json::Kind::Array) throw_decode("required must be an array");
        for (const json::Value& entry : required->elements) {
            parameters.required.push_back(decode_string(entry));
        }
    }
    return parameters;
}

ToolDefinition decode_tool_definition(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("tool must be an object");
    ToolDefinition tool;
    const json::Value* type = member(value, "type");
    if (type != nullptr && !is_null(type)) tool.type = decode_string(*type);
    const json::Value& function = decode_object_field(value, "function");
    const json::Value* name = member(function, "name");
    if (name == nullptr) throw_decode("tool function requires name");
    tool.function.name = decode_string(*name);
    const json::Value* description = member(function, "description");
    if (description == nullptr) throw_decode("tool function requires description");
    tool.function.description = decode_string(*description);
    const json::Value* parameters = member(function, "parameters");
    if (parameters == nullptr) throw_decode("tool function requires parameters");
    tool.function.parameters = decode_tool_parameters(*parameters);
    return tool;
}

}  // namespace

/** JVM subnormal spelling: the closest decimal of k significant digits where k is the smallest
 *  round-trip count floored at two ("7.0E-45", "1.401E-42"); normals stay on the shortest form. */
std::string format_with_digits(const std::string& digits, std::int64_t dec_exp, bool negative) {
    std::string out;
    out.reserve(digits.size() + 8);
    if (negative) out.push_back('-');
    if (dec_exp > -3 && dec_exp <= 7) {
        // Plain notation: 0.001 .. 9999999.0 (JVM FloatingDecimal range for Float.toString).
        if (dec_exp <= 0) {
            out += "0.";
            out.append(static_cast<std::size_t>(-dec_exp), '0');
            out += digits;
        } else if (dec_exp >= static_cast<std::int64_t>(digits.size())) {
            out += digits;
            out.append(static_cast<std::size_t>(dec_exp - static_cast<std::int64_t>(digits.size())),
                       '0');
            out += ".0";
        } else {
            out.append(digits, 0, static_cast<std::size_t>(dec_exp));
            out.push_back('.');
            out.append(digits, static_cast<std::size_t>(dec_exp), std::string::npos);
        }
    } else {
        // Computerized scientific: one integral digit, uppercase E, unsigned exponent.
        out.push_back(digits.front());
        out.push_back('.');
        if (digits.size() == 1) {
            out.push_back('0');
        } else {
            out.append(digits, 1, std::string::npos);
        }
        out.push_back('E');
        out += std::to_string(dec_exp - 1);
    }
    return out;
}

/**
 * Kotlin Float.toString(): the SHORTEST decimal that parses back to the same float, formatted
 * with the JVM's plain/scientific threshold. std::to_chars cannot be used for the digits — it
 * guarantees the shortest CHARACTER form ("467658912"), not the shortest DIGIT form the JVM
 * prints ("4.676589E8"), so the digit search rounds the exact value to k significant digits
 * (nearest, ties to even like Ryu) and re-parses the candidate until it round-trips. Subnormal
 * floats never print fewer than two significant digits on the JVM; both rules are pinned by the
 * golden traces and the JVM-captured vectors.
 */
std::string format_kotlin_float(float value) {
    if (std::isnan(value)) return "NaN";
    if (std::isinf(value)) return value < 0.0f ? "-Infinity" : "Infinity";
    if (value == 0.0f) return std::signbit(value) ? "-0.0" : "0.0";

    const bool negative = value < 0.0f;
    const long double magnitude = std::fabs(static_cast<long double>(value));
    const std::size_t min_digits =
        magnitude < static_cast<long double>(std::numeric_limits<float>::min()) ? 2 : 1;

    // e10: magnitude == 0.d1d2... x 10^(e10+1); floor(log10) can land a ulp off, so adjust.
    // Plain std::floor/log10/pow: the C99 'l'-suffixed names are not portable (absent on some
    // runners' headers), while the long double overloads are guaranteed by <cmath>.
    int e10 = static_cast<int>(std::floor(std::log10(magnitude)));
    while (std::pow(10.0L, e10) > magnitude) --e10;
    while (std::pow(10.0L, e10 + 1) <= magnitude) ++e10;

    for (std::size_t digits_count = min_digits; digits_count <= 9; ++digits_count) {
        int scale_exponent = e10 - static_cast<int>(digits_count) + 1;
        const long double scale = std::pow(10.0L, scale_exponent);
        const long double scaled = magnitude / scale;
        const long double integral = std::floor(scaled);
        long long digits_value = static_cast<long long>(integral);
        const long double fraction = scaled - integral;
        if (fraction > 0.5L) {
            ++digits_value;
        } else if (fraction == 0.5L) {
            digits_value += (digits_value % 2 == 0) ? 0 : 1;  // Ryu ties: keep the even digit
        }
        if (digits_value <= 0) continue;

        std::string digits = std::to_string(digits_value);
        if (digits.size() > digits_count) {
            // Rounding rolled the digits over (999… -> 1000…): normalize to "1" and bump the
            // exponent so 0.01f prints "0.01" instead of "0.010".
            const int bump = static_cast<int>(digits.size()) - 1;
            digits = digits.front();
            scale_exponent += bump;
        }
        std::string candidate = digits + "e" + std::to_string(scale_exponent);
        float parsed = 0.0f;
        if (parse_float(candidate, parsed) && std::fabs(parsed) == magnitude) {
            const std::int64_t dec_exp =
                static_cast<std::int64_t>(scale_exponent) + static_cast<std::int64_t>(digits.size());
            return format_with_digits(digits, dec_exp, negative);
        }
    }

    // Unreachable for any float (nine digits always round-trip): keep a defined, round-tripping
    // shape rather than failing the request.
    char buffer[64];
    const auto converted = std::to_chars(buffer, buffer + sizeof(buffer), value, std::chars_format::scientific, 8);
    if (converted.ec != std::errc{}) return "NaN";
    return std::string(buffer, converted.ptr);
}

std::string encode_base64(std::string_view bytes) {
    static constexpr char kAlphabet[] =
        "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    std::string out;
    out.reserve(((bytes.size() + 2) / 3) * 4);
    std::size_t index = 0;
    for (; index + 3 <= bytes.size(); index += 3) {
        const std::uint32_t triple = (static_cast<std::uint32_t>(
                                          static_cast<unsigned char>(bytes[index]))
                                      << 16) |
                                     (static_cast<std::uint32_t>(
                                          static_cast<unsigned char>(bytes[index + 1]))
                                      << 8) |
                                     static_cast<std::uint32_t>(
                                         static_cast<unsigned char>(bytes[index + 2]));
        out.push_back(kAlphabet[(triple >> 18) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 12) & 0x3F]);
        out.push_back(kAlphabet[(triple >> 6) & 0x3F]);
        out.push_back(kAlphabet[triple & 0x3F]);
    }
    const std::size_t remaining = bytes.size() - index;
    if (remaining == 1) {
        const std::uint32_t group = static_cast<std::uint32_t>(
                                        static_cast<unsigned char>(bytes[index]))
                                    << 16;
        out.push_back(kAlphabet[(group >> 18) & 0x3F]);
        out.push_back(kAlphabet[(group >> 12) & 0x3F]);
        out += "==";
    } else if (remaining == 2) {
        const std::uint32_t group = (static_cast<std::uint32_t>(
                                         static_cast<unsigned char>(bytes[index]))
                                     << 16) |
                                    (static_cast<std::uint32_t>(
                                         static_cast<unsigned char>(bytes[index + 1]))
                                     << 8);
        out.push_back(kAlphabet[(group >> 18) & 0x3F]);
        out.push_back(kAlphabet[(group >> 12) & 0x3F]);
        out.push_back(kAlphabet[(group >> 6) & 0x3F]);
        out.push_back('=');
    }
    return out;
}

std::string write_open_ai_message(const OpenAiMessage& message) {
    MemberList member_list;
    member_list.begin_member("role");
    append_quoted(member_list.out, message.role);
    if (message.content) {
        member_list.begin_member("content");
        write_content_parts(member_list.out, *message.content);
    }
    if (message.tool_calls) {
        member_list.begin_member("tool_calls");
        write_tool_calls(member_list.out, *message.tool_calls);
    }
    if (message.tool_call_id) {
        member_list.begin_member("tool_call_id");
        append_quoted(member_list.out, *message.tool_call_id);
    }
    if (message.reasoning_content) {
        member_list.begin_member("reasoning_content");
        append_quoted(member_list.out, *message.reasoning_content);
    }
    return member_list.finish();
}

std::string write_open_ai_message_list(const std::vector<OpenAiMessage>& messages) {
    std::string out = "[";
    bool first = true;
    for (const OpenAiMessage& message : messages) {
        if (!first) out.push_back(',');
        first = false;
        out += write_open_ai_message(message);
    }
    out.push_back(']');
    return out;
}

std::string write_open_ai_chat_request(const OpenAiChatRequest& request) {
    MemberList out;
    out.begin_member("model");
    append_quoted(out.out, request.model);
    out.begin_member("messages");
    out.out += write_open_ai_message_list(request.messages);
    out.begin_member("stream");
    out.out += request.stream ? "true" : "false";
    if (request.stream_options) {
        out.begin_member("stream_options");
        MemberList options;
        options.begin_member("include_usage");
        options.out += request.stream_options->include_usage ? "true" : "false";
        options.out.push_back('}');
        out.out += options.out;
    }
    if (request.tools) {
        out.begin_member("tools");
        out.out.push_back('[');
        bool first_tool = true;
        for (const ToolDefinition& tool : *request.tools) {
            if (!first_tool) out.out.push_back(',');
            first_tool = false;
            write_tool_definition(out.out, tool);
        }
        out.out.push_back(']');
    }
    if (request.reasoning_effort) {
        out.begin_member("reasoning_effort");
        append_quoted(out.out, *request.reasoning_effort);
    }
    if (request.thinking) {
        out.begin_member("thinking");
        MemberList thinking;
        thinking.begin_member("type");
        append_quoted(thinking.out, request.thinking->type);
        thinking.out.push_back('}');
        out.out += thinking.out;
    }
    if (request.enable_thinking) {
        out.begin_member("enable_thinking");
        out.out += *request.enable_thinking ? "true" : "false";
    }
    if (request.thinking_budget) {
        out.begin_member("thinking_budget");
        write_int(out.out, *request.thinking_budget);
    }
    if (request.reasoning) {
        out.begin_member("reasoning");
        MemberList reasoning;
        if (request.reasoning->effort) {
            reasoning.begin_member("effort");
            append_quoted(reasoning.out, *request.reasoning->effort);
        }
        if (request.reasoning->summary) {
            reasoning.begin_member("summary");
            append_quoted(reasoning.out, *request.reasoning->summary);
        }
        if (request.reasoning->max_tokens) {
            reasoning.begin_member("max_tokens");
            write_int(reasoning.out, *request.reasoning->max_tokens);
        }
        if (request.reasoning->enabled) {
            reasoning.begin_member("enabled");
            reasoning.out += *request.reasoning->enabled ? "true" : "false";
        }
        reasoning.out.push_back('}');
        out.out += reasoning.out;
    }
    if (request.include_reasoning) {
        out.begin_member("include_reasoning");
        out.out += *request.include_reasoning ? "true" : "false";
    }
    if (request.plugins) {
        out.begin_member("plugins");
        out.out.push_back('[');
        bool first_plugin = true;
        for (const OpenAiPlugin& plugin : *request.plugins) {
            if (!first_plugin) out.out.push_back(',');
            first_plugin = false;
            MemberList entry;
            entry.begin_member("id");
            append_quoted(entry.out, plugin.id);
            entry.out.push_back('}');
            out.out += entry.out;
        }
        out.out.push_back(']');
    }
    if (request.service_tier) {
        out.begin_member("service_tier");
        append_quoted(out.out, *request.service_tier);
    }
    if (request.temperature) {
        out.begin_member("temperature");
        append_float(out.out, *request.temperature);
    }
    if (request.max_tokens) {
        out.begin_member("max_tokens");
        write_int(out.out, *request.max_tokens);
    }
    if (request.top_p) {
        out.begin_member("top_p");
        append_float(out.out, *request.top_p);
    }
    if (request.frequency_penalty) {
        out.begin_member("frequency_penalty");
        append_float(out.out, *request.frequency_penalty);
    }
    if (request.presence_penalty) {
        out.begin_member("presence_penalty");
        append_float(out.out, *request.presence_penalty);
    }
    if (request.prompt_cache_key) {
        out.begin_member("prompt_cache_key");
        append_quoted(out.out, *request.prompt_cache_key);
    }
    return out.finish();
}

OpenAiChatRequest decode_open_ai_chat_request(const json::Value& value) {
    if (value.kind != json::Kind::Object) throw_decode("expected a JSON object");
    OpenAiChatRequest request;

    const json::Value* model = member(value, "model");
    if (model == nullptr) throw_decode("field 'model' is required");
    request.model = decode_string(*model);

    const json::Value* messages = member(value, "messages");
    if (messages == nullptr || messages->kind != json::Kind::Array) {
        throw_decode("field 'messages' is required");
    }
    for (const json::Value& entry : messages->elements) {
        request.messages.push_back(decode_message(entry));
    }

    if (const auto stream = decode_optional_bool(value, "stream")) request.stream = *stream;

    const json::Value* stream_options = member(value, "stream_options");
    if (stream_options != nullptr && !is_null(stream_options)) {
        if (stream_options->kind != json::Kind::Object) throw_decode("stream_options must be an object");
        OpenAiStreamOptions options;
        if (const auto include_usage =
                decode_optional_bool(*stream_options, "include_usage")) {
            options.include_usage = *include_usage;
        }
        request.stream_options = options;
    }

    const json::Value* tools = member(value, "tools");
    if (tools != nullptr && !is_null(tools)) {
        if (tools->kind != json::Kind::Array) throw_decode("tools must be an array");
        std::vector<ToolDefinition> decoded;
        decoded.reserve(tools->elements.size());
        for (const json::Value& entry : tools->elements) decoded.push_back(decode_tool_definition(entry));
        request.tools = std::move(decoded);
    }

    request.reasoning_effort = decode_optional_string(value, "reasoning_effort");

    const json::Value* thinking = member(value, "thinking");
    if (thinking != nullptr && !is_null(thinking)) {
        if (thinking->kind != json::Kind::Object) throw_decode("thinking must be an object");
        const json::Value* type = member(*thinking, "type");
        if (type == nullptr) throw_decode("thinking requires type");
        request.thinking = OpenAiThinking{decode_string(*type)};
    }

    request.enable_thinking = decode_optional_bool(value, "enable_thinking");
    request.thinking_budget = decode_optional_int(value, "thinking_budget");

    const json::Value* reasoning = member(value, "reasoning");
    if (reasoning != nullptr && !is_null(reasoning)) {
        if (reasoning->kind != json::Kind::Object) throw_decode("reasoning must be an object");
        OpenAiReasoning decoded;
        decoded.effort = decode_optional_string(*reasoning, "effort");
        decoded.summary = decode_optional_string(*reasoning, "summary");
        decoded.max_tokens = decode_optional_int(*reasoning, "max_tokens");
        decoded.enabled = decode_optional_bool(*reasoning, "enabled");
        request.reasoning = decoded;
    }

    request.include_reasoning = decode_optional_bool(value, "include_reasoning");

    const json::Value* plugins = member(value, "plugins");
    if (plugins != nullptr && !is_null(plugins)) {
        if (plugins->kind != json::Kind::Array) throw_decode("plugins must be an array");
        std::vector<OpenAiPlugin> decoded;
        decoded.reserve(plugins->elements.size());
        for (const json::Value& entry : plugins->elements) {
            if (entry.kind != json::Kind::Object) throw_decode("plugin must be an object");
            const json::Value* id = member(entry, "id");
            if (id == nullptr) throw_decode("plugin requires id");
            decoded.push_back(OpenAiPlugin{decode_string(*id)});
        }
        request.plugins = std::move(decoded);
    }

    request.service_tier = decode_optional_string(value, "service_tier");
    request.temperature = decode_optional_float(value, "temperature");
    request.max_tokens = decode_optional_int(value, "max_tokens");
    request.top_p = decode_optional_float(value, "top_p");
    request.frequency_penalty = decode_optional_float(value, "frequency_penalty");
    request.presence_penalty = decode_optional_float(value, "presence_penalty");
    request.prompt_cache_key = decode_optional_string(value, "prompt_cache_key");
    return request;
}

}  // namespace agora::core::openai
