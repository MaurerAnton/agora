// Port of api/util/MalformedToolCall.kt.
//
// The damaged call is replaced by one pairable call under a reserved name; the arguments carry the
// cause plus the original name and arguments (truncated), and the result text explains the failure
// back to the model.
#include "agora/core/malformed_tool_call.hpp"

#include <optional>
#include <string>
#include <string_view>

#include "agora/core/json.hpp"

namespace agora::core::malformed_tool {

namespace {

constexpr std::size_t kOriginalNameMaxChars = 200;
constexpr std::size_t kOriginalArgumentsMaxChars = 2'000;

std::string truncate(std::string_view text, std::size_t max_chars) {
    if (text.size() <= max_chars) return std::string(text);
    return std::string(text.substr(0, max_chars));
}

}  // namespace

MalformedToolCallRequest malformed_tool_call_request(
    std::string_view cause, const std::optional<std::string>& original_name,
    const std::optional<std::string>& original_arguments,
    const std::optional<std::string>& stream_key, const std::optional<std::string>& signature,
    std::string_view suffix) {
    json::Value arguments;
    arguments.kind = json::Kind::Object;
    arguments.members.emplace_back("error", [&] {
        json::Value value;
        value.kind = json::Kind::String;
        value.text = std::string(cause);
        return value;
    }());
    if (original_name.has_value() && !original_name->empty()) {
        arguments.members.emplace_back("original_name", [&] {
            json::Value value;
            value.kind = json::Kind::String;
            value.text = truncate(*original_name, kOriginalNameMaxChars);
            return value;
        }());
    }
    if (original_arguments.has_value() && !original_arguments->empty()) {
        arguments.members.emplace_back("original_arguments", [&] {
            json::Value value;
            value.kind = json::Kind::String;
            value.text = truncate(*original_arguments, kOriginalArgumentsMaxChars);
            return value;
        }());
    }

    MalformedToolCallRequest request;
    request.id = "call_malformed_" + std::string(suffix);
    request.name = std::string(kMalformedToolCallName);
    request.arguments = json::to_kotlin_string(arguments);
    request.signature = signature;
    request.stream_key = stream_key.has_value() && !json::is_blank(*stream_key)
                             ? *stream_key
                             : "malformed_tool_" + std::string(suffix);
    return request;
}

std::string malformed_tool_call_result_text(std::string_view arguments) {
    // The Kotlin source parses the arguments leniently: a malformed document simply yields no
    // fields, so the result text carries the generic explanation only.
    std::optional<std::string> error;
    std::optional<std::string> original_name;
    std::optional<std::string> original_arguments;
    const json::ParseResult parsed = json::parse(arguments);
    if (parsed.value.has_value() && parsed.value->kind == json::Kind::Object) {
        for (const json::Member& member : parsed.value->members) {
            // JsonPrimitive.content: a string contributes its text, a literal its raw text
            // (JsonNull included), an object or an array is not a primitive at all.
            if (member.value.kind != json::Kind::String && member.value.kind != json::Kind::Literal) {
                continue;
            }
            if (member.key == "error" && !error.has_value()) {
                error = member.value.text;
            } else if (member.key == "original_name" && !original_name.has_value()) {
                original_name = member.value.text;
            } else if (member.key == "original_arguments" && !original_arguments.has_value()) {
                original_arguments = member.value.text;
            }
        }
    }

    std::string out = "Error: the previous tool call was malformed and was not executed";
    if (error.has_value()) out += ". Cause: " + *error;
    if (original_name.has_value()) out += ". Tool name as sent: " + *original_name;
    if (original_arguments.has_value()) out += ". Arguments as sent: " + *original_arguments;
    out +=
        ". Re-issue the call with one of the offered tool names and a complete JSON object as "
        "arguments. If the arguments were cut off, send a smaller payload.";
    return out;
}

}  // namespace agora::core::malformed_tool
