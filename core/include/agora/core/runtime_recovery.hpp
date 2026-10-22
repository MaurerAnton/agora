#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/conversation_runtime.hpp"
#include "agora/core/run_branch_selection.hpp"
#include "agora/core/run_recovery.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::runtime_recovery {

/**
 * Port of data/local/ChatDaoRuntimeRecovery.kt: the @Transaction body of
 * ChatDao.recoverConversationRuntime. One pass repairs a conversation's runtime after a process
 * death: it folds the live Run back through the runtime reducer, rewrites the in-flight message
 * checkpoints (status + incomplete tool JSON) with RunRecoveryPolicy, validates the durable
 * run-branch projection with the already-ported RunBranchSelectionIntegrity, stops stuck rows and
 * touches the conversation's export watermark exactly once when anything changed.
 *
 * The nine ChatDao queries are the boundary (ChatDao.kt is another module): RecoveryStore carries
 * the same operations the extension calls, and the store implementation owns their SQL.
 */

/** The MessageEntity columns the body reads, with the converter already applied to `images`. */
struct RecoveryMessage {
    std::string id;
    run_recovery::Participant participant = run_recovery::Participant::model;
    run_recovery::MessageStatus status = run_recovery::MessageStatus::success;
    std::optional<std::string> tool_call_json;

    // MessageStreamCheckpoint fields the body copies back into the update.
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
    std::optional<std::int64_t> thought_time_ms;
};

/** The durable streaming checkpoint the body writes when a row changed (id keys the UPDATE). */
struct MessageCheckpoint {
    std::string id;
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
    std::optional<std::int64_t> thought_time_ms;
    std::optional<std::string> tool_call_json;
};

/** The conversation row the body reads (getConversation). */
struct ConversationSnapshot {
    std::string id;
    std::optional<std::string> selected_run_branches_json;
};

/** The live Run row: the fields RunRecoverySnapshot and the terminalize call consume. */
struct RecoveryRun {
    std::string id;
    conversation_runtime::RunStatus status = conversation_runtime::RunStatus::Active;
    int current_pass = 0;
};

/** The nine ChatDao operations plus Room's transaction nesting. */
class RecoveryStore {
public:
    virtual ~RecoveryStore() = default;

    // -- Room's @Transaction wrapper around the body ----------------------------------------
    virtual void begin_transaction() = 0;
    virtual void set_transaction_successful() = 0;
    virtual void end_transaction() = 0;

    // -- ChatDao surface --------------------------------------------------------------------
    virtual std::optional<ConversationSnapshot> get_conversation(std::string_view conversation_id) = 0;

    virtual std::optional<RecoveryRun> get_live_run(std::string_view conversation_id) = 0;

    virtual std::vector<RecoveryMessage> get_messages_for_runs(
        const std::vector<std::string>& run_ids) = 0;

    /** `@Update(entity = MessageEntity::class)`: UPDATE by primary key over the projection. */
    virtual std::int64_t update_message_checkpoint(const MessageCheckpoint& checkpoint) = 0;

    virtual std::int64_t terminalize_live_run(std::string_view run_id,
                                              conversation_runtime::RunStatus status,
                                              conversation_runtime::RunEndReason reason,
                                              std::int64_t at) = 0;

    virtual std::int64_t compare_and_set_run_branch_selections(std::string_view conversation_id,
                                                              std::string_view expected,
                                                              std::string_view replacement) = 0;

    virtual std::int64_t stop_stuck_messages_for_conversation(std::string_view conversation_id) = 0;

    virtual std::int64_t touch_conversation_data(std::string_view conversation_id,
                                                std::int64_t at) = 0;

    virtual std::vector<run_branch_selection::RunRef> get_runs_for_conversation_snapshot(
        std::string_view conversation_id) = 0;
};

/**
 * ChatDao.executeRuntimeRecovery, called through the @Transaction wrapper
 * (ChatDao.recoverConversationRuntime). Returns the changed-row count the body reports.
 */
std::int64_t execute_runtime_recovery(RecoveryStore& store, std::string_view conversation_id,
                                      std::int64_t at);

}  // namespace agora::core::runtime_recovery
