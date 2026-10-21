#pragma once

#include <optional>
#include <string>
#include <vector>

#include "agora/core/message_preparation.hpp"

namespace agora::core::run_ui {

// The chat model is the already-ported message_preparation subset.
namespace prep = agora::core::prep;

/**
 * Port of model/MessageGenerationBoundary.kt and model/RunUiProjection.kt: the message generation
 * boundaries (which rows belong to one generation) and the UI affordances derived from them.
 *
 * The two files share the ChatMessage model, so the port reuses the already-ported
 * prep::ChatMessage instead of copying the model again.
 */

/** model/MessageGenerationBoundary.kt::MessageGenerationBoundary. */
struct GenerationBoundary {
    std::vector<prep::ChatMessage> messages;
    const prep::ChatMessage* input = nullptr;
    const prep::ChatMessage* first_assistant = nullptr;
    const prep::ChatMessage* last_assistant = nullptr;
};

/** model/RunUiProjection.kt::RunMessagePresentation. */
struct MessagePresentation {
    bool show_actions = false;
    std::optional<std::string> copy_text;
    std::optional<std::string> delete_target_message_id;
    bool show_branch_selector = false;
    int branch_index = 0;
    int total_branches = 1;
    std::optional<std::string> branch_anchor_parent_id;
    std::optional<std::string> branch_anchor_message_id;
};

/** MessageGenerationBoundaryResolver.resolve(visibleMessages). */
std::vector<GenerationBoundary> resolve_boundaries(
    const std::vector<prep::ChatMessage>& visible_messages);

/** MessageGenerationBoundaryResolver.isRealUser. */
bool is_real_user(const prep::ChatMessage& message);

/** MessageGenerationBoundaryResolver.isOrdinaryAssistant. */
bool is_ordinary_assistant(const prep::ChatMessage& message);

/** RunUiProjection.project(visibleMessages, allMessages). */
std::vector<std::pair<std::string, MessagePresentation>> project(
    const std::vector<prep::ChatMessage>& visible_messages,
    const std::vector<prep::ChatMessage>& all_messages);

}  // namespace agora::core::run_ui
