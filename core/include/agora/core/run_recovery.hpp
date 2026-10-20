#pragma once

#include <array>
#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

namespace agora::core::run_recovery {

/** Port of model/ChatMessage.kt's Participant enum. */
enum class Participant { user, model, error };

/** Port of model/ChatMessage.kt's MessageStatus enum. */
enum class MessageStatus {
    transcribing,
    sending,
    thinking,
    tool_calling,
    success,
    stopped,
    error,
};

/** Port of model/ChatMessage.kt's ToolImageAttachment. */
struct ToolImageAttachment {
    std::string path;
    std::string mime_type;
    std::int64_t size_bytes = 0;
    std::optional<int> width;
    std::optional<int> height;
    std::string sha256;
};

/** Port of model/ChatMessage.kt's StreamingTextDelta. */
struct StreamingTextDelta {
    std::int64_t sequence = 0;
    int code_point_count = 0;
};

/**
 * Port of model/ChatMessage.kt's MessageSegment.
 *
 * Every field of the Kotlin data class is represented, including the two @Transient ones, because
 * stopIncompleteTools uses copy(toolState = ...) and therefore preserves all of them. Field order
 * matches the Kotlin declaration order.
 *
 * responseOutputItems holds each JsonObject re-serialized compactly in source-key order, which is
 * how kotlinx decodes and re-encodes them (a JsonObject is a LinkedHashMap).
 */
struct MessageSegment {
    std::string type;
    std::string content;
    std::optional<std::string> tool_name;
    std::optional<std::string> tool_args;
    std::optional<std::string> tool_result;
    std::optional<std::string> tool_call_id;
    std::optional<std::string> signature;
    std::optional<std::string> signature_provider;
    std::optional<std::int64_t> duration_ms;
    std::optional<std::string> tool_state;
    std::optional<std::string> tool_progress;
    std::optional<std::string> tool_target;
    std::optional<std::string> tool_display_name;
    std::optional<std::string> tool_result_text;
    std::optional<std::string> tool_structured_result;
    std::vector<ToolImageAttachment> tool_images;
    std::optional<std::string> tool_image_request_key;
    std::optional<std::string> tool_transcription;
    std::vector<std::string> response_output_items;
    std::optional<std::string> response_output_item_provider;
    std::optional<std::string> error_code;
    std::vector<StreamingTextDelta> streaming_text_deltas;
};

/** Port of model/ChatMessage.kt's ToolExecutionStates object. */
namespace tool_execution_states {

inline constexpr std::string_view calling = "calling";
inline constexpr std::string_view running = "running";
inline constexpr std::string_view succeeded = "succeeded";
inline constexpr std::string_view empty = "empty";
inline constexpr std::string_view failed = "failed";
inline constexpr std::string_view stopped = "stopped";
inline constexpr std::string_view background_running = "background_running";

/** ToolExecutionStates.TERMINAL, in the Kotlin set's literal order. */
inline constexpr std::array<std::string_view, 5> terminal = {
    succeeded,
    empty,
    failed,
    stopped,
    background_running,
};

/** `state in ToolExecutionStates.TERMINAL`; nullopt (Kotlin null) is not terminal. */
bool is_terminal(const std::optional<std::string>& state);

}  // namespace tool_execution_states

/**
 * Port of RunRecoveryPolicy.recoverMessageStatus.
 *
 * A model message that was mid-flight when the Android process died cannot still be sending,
 * thinking, calling a tool or transcribing. Terminal states stay authoritative, and another
 * participant's status is never touched.
 */
MessageStatus recover_message_status(Participant participant, MessageStatus status);

/**
 * Port of RunRecoveryPolicy.stopIncompleteTools.
 *
 * Tool segments whose lifecycle is absent or non-terminal are copied with toolState = "stopped".
 * Every other field, and every non-tool segment, is preserved exactly; the input order is kept.
 */
std::vector<MessageSegment> stop_incomplete_tools(const std::vector<MessageSegment>& segments);

/**
 * Port of RunRecoveryPolicy.stopIncompleteToolsJson.
 *
 * Strictly parses raw with kotlinx's parseToJsonElement acceptance rules. A non-array root or a
 * parse failure yields nullopt. Each object element whose "type" primitive content is exactly
 * "tool" and whose "toolState" primitive content is absent or non-terminal is rewritten to
 * "toolState":"stopped"; non-object elements are carried through unchanged. When nothing needs
 * rewriting the original raw text is returned byte for byte (whitespace included); otherwise the
 * whole array is re-serialized as kotlinx JsonArray.toString() does: compact, source key order,
 * last duplicate key wins at its first position, and the rewritten key appended when absent.
 */
std::optional<std::string> stop_incomplete_tools_json(std::string_view raw);

}  // namespace agora::core::run_recovery
