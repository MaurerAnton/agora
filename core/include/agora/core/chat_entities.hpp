#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

#include "agora/core/run_recovery.hpp"

namespace agora::core {

/**
 * MessageEntity (data/local/ChatEntities.kt): no init constraints, full row shape.
 *
 * Shared by every DAO port that reads or writes the messages table; Room maps the enum columns
 * through MessageConverters (Enum.valueOf on the stored name) and the `images` list through the
 * app's JSON-array converter.
 */
struct MessageEntity {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_id;
    std::string text;
    std::vector<std::string> images;
    std::optional<std::string> thoughts;
    std::optional<std::string> thought_title;
    int token_count = 0;
    std::optional<int> input_token_count;
    std::optional<int> cached_input_token_count;
    std::optional<int> cache_write_input_token_count;
    std::optional<int> uncached_input_token_count;
    std::optional<int> output_token_count;
    std::optional<int> reasoning_token_count;
    std::optional<std::int64_t> generation_duration_ms;
    run_recovery::MessageStatus status = run_recovery::MessageStatus::success;
    run_recovery::Participant participant = run_recovery::Participant::model;
    std::int64_t timestamp = 0;
    std::optional<std::int64_t> thought_time_ms;
    std::optional<std::string> model_name;
    std::optional<std::string> tool_call_json;
    std::optional<std::string> attachment_meta;
    std::string run_id;
    std::int64_t run_sequence = -1;  // MessageEntity.UNASSIGNED_RUN_SEQUENCE
    std::optional<int> consumed_at_pass;
    std::optional<std::string> source_json;
};

/** ChatEntity (data/local/ChatEntities.kt): the conversation row, full declared shape. */
struct ChatEntity {
    std::string id;
    std::string title;
    std::int64_t last_updated = 0;
    std::int64_t data_changed_at = 0;
    std::optional<std::string> selected_branches_json;
    std::optional<std::string> system_prompt_id;
    std::optional<std::string> model_id;
    std::optional<std::string> task_id;
    std::string origin = "user";
    bool graduated = false;
    std::string draft_text;
    std::optional<std::string> draft_attachments;
    std::optional<std::string> selected_run_branches_json;
    bool has_unread_generation = false;
    bool is_pinned = false;
};

}  // namespace agora::core
