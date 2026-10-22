#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conversation_runtime.hpp"
#include "agora/core/message_preparation.hpp"
#include "agora/core/message_source.hpp"

namespace agora::core::generation_policy {

// The chat model, the durable run states and the structured source are already-ported modules.
namespace prep = agora::core::prep;
namespace runtime = agora::core::conversation_runtime;
namespace source_model = agora::core::message_source;

/**
 * Port of viewmodel/GenerationPolicies.kt and viewmodel/AutomaticMessageApiProjection.kt: the
 * rules that decide how a provider generation ends, how the UI is throttled, and which history the
 * provider actually receives.
 *
 * The two Kotlin files are lifted verbatim by the golden stand (only the visibility is widened),
 * so this port follows them line by line. The chat model is the already-ported
 * prep::ChatMessage; the structured source of an automatic message is the
 * already-ported source_model::Source.
 */

/** viewmodel/GenerationPolicies.kt::GenerationTerminalDisposition. */
struct TerminalDisposition {
    runtime::RunStatus run_status = runtime::RunStatus::Completed;
    runtime::RunEndReason end_reason = runtime::RunEndReason::ModelCompleted;
    bool mark_conversation_unread = false;
};

/** shouldPostGenerationTerminalNotification(messageStatus, hasPendingGuidance, ...). */
bool should_post_generation_terminal_notification(
    prep::MessageStatus message_status, bool has_pending_guidance,
    bool is_context_compact, bool app_in_foreground,
    const std::optional<bool>& conversation_visible);

/** generationTerminalDisposition(messageStatus, hasPendingGuidance, conversationVisible). */
TerminalDisposition generation_terminal_disposition(
    prep::MessageStatus message_status, bool has_pending_guidance,
    const std::optional<bool>& conversation_visible = std::nullopt);

/** StreamingCheckpointGate: throttles durable stream snapshots, boundaries force a write. */
class StreamingCheckpointGate {
public:
    explicit StreamingCheckpointGate(std::int64_t interval_ms = 1'000);

    bool should_checkpoint(std::int64_t now_ms, bool force = false);

private:
    std::int64_t interval_ms_;
    std::optional<std::int64_t> last_checkpoint_at_;
};

/** StreamingUiUpdateGate: throttles the visible snapshot cadence; a clock rollback is due. */
class StreamingUiUpdateGate {
public:
    explicit StreamingUiUpdateGate(std::int64_t interval_ms = 50);

    bool is_due(std::int64_t now_ms);
    void record_published(std::int64_t now_ms);
    void reset();

private:
    std::int64_t interval_ms_;
    std::optional<std::int64_t> last_published_at_;
};

/** toolRoundThoughtSegments(segments, fromIndex). */
std::vector<prep::MessageSegment> tool_round_thought_segments(
    const std::vector<prep::MessageSegment>& segments, int from_index);

/**
 * ToolRoundHistoryCompactor: removes only the strict cumulative thought prefix written by
 * older builds.
 */
class ToolRoundHistoryCompactor {
public:
    std::vector<prep::MessageSegment> compact(
        const std::string& run_id,
        const std::vector<prep::MessageSegment>& segments);

private:
    std::vector<std::pair<std::string, std::vector<prep::MessageSegment>>> history_;
};

/** applyMessageTemplatesToMessages(messages, userPrepend, userPostpend, assistantPrepend, assistantPostpend). */
std::vector<prep::ChatMessage> apply_message_templates_to_messages(
    const std::vector<prep::ChatMessage>& messages,
    const std::optional<std::string>& user_prepend = std::nullopt,
    const std::optional<std::string>& user_postpend = std::nullopt,
    const std::optional<std::string>& assistant_prepend = std::nullopt,
    const std::optional<std::string>& assistant_postpend = std::nullopt);

/** applyUserTemplateToMessages(messages, prepend, postpend). */
std::vector<prep::ChatMessage> apply_user_template_to_messages(
    const std::vector<prep::ChatMessage>& messages,
    const std::optional<std::string>& prepend = std::nullopt,
    const std::optional<std::string>& postpend = std::nullopt);

/** automaticMessageApiText(source, text): the XML envelope the model receives. */
std::string automatic_message_api_text(const source_model::Source& source, std::string_view text);

/** projectAutomaticMessagesForApi(messages). */
std::vector<prep::ChatMessage> project_automatic_messages_for_api(
    const std::vector<prep::ChatMessage>& messages);

/** projectGenerationInputMessages(messages, includeImages, templates, initialUserPrompt). */
std::vector<prep::ChatMessage> project_generation_input_messages(
    const std::vector<prep::ChatMessage>& messages, bool include_images,
    const std::optional<std::string>& user_prepend = std::nullopt,
    const std::optional<std::string>& user_postpend = std::nullopt,
    const std::optional<std::string>& assistant_prepend = std::nullopt,
    const std::optional<std::string>& assistant_postpend = std::nullopt,
    const std::optional<std::string>& initial_user_prompt = std::nullopt);

}  // namespace agora::core::generation_policy
