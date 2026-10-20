#include "agora/core/run_recovery.hpp"

#include <string>
#include <utility>

#include "agora/core/json.hpp"

namespace agora::core::run_recovery {
namespace {

namespace json = agora::core::json;
using json::Kind;

const json::Value* last_member(const json::Value& object, std::string_view name) {
    const json::Value* found = nullptr;
    for (const json::Member& member : object.members) {
        if (member.key == name) found = &member.value;
    }
    return found;
}

/**
 * `(element as? JsonPrimitive)?.contentOrNull`.
 *
 * A JsonNull is a JsonPrimitive in kotlinx but contentOrNull maps it to null; unquoted literals
 * (numbers, booleans and bare TC_OTHER runs) keep their raw content, and an array or object is not
 * a JsonPrimitive at all. `nullopt` is therefore Kotlin's null, not the string "null".
 */
std::optional<std::string> primitive_content_or_null(const json::Value* value) {
    if (value == nullptr) return std::nullopt;
    if (value->kind == Kind::String) return value->text;
    if (value->kind != Kind::Literal) return std::nullopt;
    if (value->text == "null") return std::nullopt;
    return value->text;
}

}  // namespace

namespace tool_execution_states {

bool is_terminal(const std::optional<std::string>& state) {
    if (!state.has_value()) return false;
    for (const std::string_view candidate : terminal) {
        if (*state == candidate) return true;
    }
    return false;
}

}  // namespace tool_execution_states

MessageStatus recover_message_status(Participant participant, MessageStatus status) {
    if (participant != Participant::model) return status;
    switch (status) {
        case MessageStatus::sending:
        case MessageStatus::thinking:
        case MessageStatus::tool_calling:
        case MessageStatus::transcribing:
            return MessageStatus::stopped;
        case MessageStatus::success:
        case MessageStatus::stopped:
        case MessageStatus::error:
            return status;
    }
    return status;
}

std::vector<MessageSegment> stop_incomplete_tools(const std::vector<MessageSegment>& segments) {
    std::vector<MessageSegment> recovered = segments;
    for (MessageSegment& segment : recovered) {
        if (segment.type == "tool" &&
            !tool_execution_states::is_terminal(segment.tool_state)) {
            segment.tool_state = std::string(tool_execution_states::stopped);
        }
    }
    return recovered;
}

std::optional<std::string> stop_incomplete_tools_json(std::string_view raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value.has_value() || parsed.value->kind != Kind::Array) return std::nullopt;

    json::Value recovered = *parsed.value;
    bool changed = false;
    for (json::Value& element : recovered.elements) {
        if (element.kind != Kind::Object) continue;

        const std::optional<std::string> type =
            primitive_content_or_null(last_member(element, "type"));
        if (type != "tool") continue;

        const std::optional<std::string> state =
            primitive_content_or_null(last_member(element, "toolState"));
        if (tool_execution_states::is_terminal(state)) continue;

        changed = true;
        // Kotlin: JsonObject(segment + ("toolState" to JsonPrimitive(STOPPED))). Appending an
        // effective member is indistinguishable in the serialized result: the compact writer
        // uses the first position of a key and the last assigned value, which is exactly
        // LinkedHashMap.plus(pair) semantics.
        json::Value stopped;
        stopped.kind = Kind::String;
        stopped.text = std::string(tool_execution_states::stopped);
        element.members.push_back(json::Member{"toolState", std::move(stopped)});
    }

    if (!changed) return std::string(raw);
    return json::to_kotlin_string(recovered);
}

}  // namespace agora::core::run_recovery
