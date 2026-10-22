#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>

namespace agora::core::malformed_tool {

/**
 * Port of api/util/MalformedToolCall.kt: the stand-in for a tool call the model emitted in a form
 * that cannot be executed or paired.
 *
 * Instead of failing the whole run, the damaged call is replaced by one pairable call under a
 * reserved name; the tool executor never runs it and answers with an error result that explains
 * what was wrong.
 *
 * The random UUID suffix is entropy: the port takes it as a parameter (the caller owns the
 * generator) instead of reading the system random source behind the caller's back.
 */

/** The reserved tool name of the stand-in. */
inline constexpr std::string_view kMalformedToolCallName = "agora_malformed_tool_call";

/** The stand-in request, mirroring api/StreamEvent.kt::ToolCallRequest. */
struct MalformedToolCallRequest {
    std::string id;
    std::string name;
    std::string arguments;
    std::optional<std::string> signature;
    std::string stream_key;
};

/**
 * malformedToolCallRequest(cause, originalName, originalArguments, streamKey, signature).
 *
 * `suffix` is the entropy of the random UUID (the Kotlin source uses UUID.randomUUID() with the
 * dashes removed); the caller supplies it.
 */
MalformedToolCallRequest malformed_tool_call_request(
    std::string_view cause, const std::optional<std::string>& original_name = std::nullopt,
    const std::optional<std::string>& original_arguments = std::nullopt,
    const std::optional<std::string>& stream_key = std::nullopt,
    const std::optional<std::string>& signature = std::nullopt, std::string_view suffix = "");

/** malformedToolCallResultText(arguments): the error text the model receives. */
std::string malformed_tool_call_result_text(std::string_view arguments);

}  // namespace agora::core::malformed_tool
