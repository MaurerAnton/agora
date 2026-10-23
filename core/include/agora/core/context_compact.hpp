#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/chat_entities.hpp"
#include "agora/core/conversation_runtime.hpp"
#include "agora/core/run_branch_selection.hpp"
#include "agora/core/run_recovery.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::context_compact {

/**
 * Port of data/local/ChatContextCompactDao.kt: the atomic Context-Compact graph contract —
 * beginning a Recompact (swapping a terminal Compact's dedicated Run for a fresh active one while
 * keeping the message's identity and graph position) and removing a Compact (reparenting its
 * children, dropping its row and embeddings, and collapsing both branch projections).
 *
 * The file is a plain interface inherited by ChatDao rather than a @Dao, and every one of its 21
 * members is ported here: the SQL text comes from the @Query annotations, the two @Transaction
 * bodies are transliterated, and Room itself is replaced by the SqlExecutor boundary.
 *
 * `System.currentTimeMillis()` inside removeContextCompact is the one clock read; the port takes
 * it as a parameter (the golden stand masks it in the table dumps).
 */

/** MessageEntity is the messages-table row shared with the other DAO ports. */
using MessageEntity = ::agora::core::MessageEntity;

/** RunEntity (data/local/RunEntity.kt), validated on construction like the Kotlin init block. */
struct RunEntity {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    conversation_runtime::RunStatus status = conversation_runtime::RunStatus::Active;
    std::optional<std::int64_t> active_slot;
    std::int64_t started_at = 0;
    std::int64_t last_checkpoint_at = 0;
    std::optional<std::int64_t> stop_requested_at;
    std::optional<std::int64_t> ended_at;
    std::optional<conversation_runtime::RunEndReason> end_reason;
    int current_pass = 0;
    bool legacy_ambiguous = false;

    /** RunEntity's init block: every `require`, in source order. */
    void validate() const;
};

/** The conversation row the compact transactions read (the stand surface). */
struct ConversationEntry {
    std::string id;
    std::optional<std::string> selected_branches_json;
    std::optional<std::string> selected_run_branches_json;
};

/**
 * The two private helpers of the compact file: a LinkedHashMap<String?, String> round trip where
 * a decode failure (or a null column) yields an EMPTY map rather than null — which is what makes
 * removeContextCompact rewrite a malformed projection to "{}" instead of skipping it.
 */
using SelectionMap = run_branch_selection::SelectionMap;

SelectionMap decode_compact_selections(const std::optional<std::string>& raw);

std::string encode_compact_selections(const SelectionMap& selections);

/** LinkedHashMap selection helpers: get/put/remove with a nullable key. */
std::optional<std::string> selection_get(const SelectionMap& selections,
                                         const std::optional<std::string>& key);

std::optional<std::string> selection_remove(SelectionMap& selections,
                                            const std::optional<std::string>& key);

/** put: an existing key keeps its position, a new key is appended (LinkedHashMap semantics). */
void selection_put(SelectionMap& selections, std::optional<std::string> key, std::string value);

/** Kotlin Map.equals: order independent, so two decodes of reordered-but-equal JSON match. */
bool selections_equal(const SelectionMap& left, const SelectionMap& right);

/** All 21 members of ChatContextCompactDao over a SqlExecutor (Room replaced by the boundary). */
class ContextCompactDao {
public:
    explicit ContextCompactDao(SqlExecutor& sql) : sql_(sql), stack_(sql) {}

    // -- @Transaction nesting (exposed for run_in_transaction) --------------------------------

    void begin_transaction();
    void set_transaction_successful();
    void end_transaction();

    // -- plain members -----------------------------------------------------------------------

    std::optional<ConversationEntry> get_conversation(std::string_view conversation_id);

    /** @Insert(onConflict = ABORT) */
    void insert_message(const MessageEntity& message);

    std::optional<MessageEntity> get_message(std::string_view message_id);

    /** @Insert(onConflict = ABORT) */
    void insert_run(const RunEntity& run);

    std::optional<RunEntity> get_run(std::string_view run_id);

    std::vector<MessageEntity> get_messages_for_runs(const std::vector<std::string>& run_ids);

    std::int64_t delete_run(std::string_view run_id);

    void delete_embeddings_by_message_ids(const std::vector<std::string>& message_ids);

    std::int64_t update_selections_for_run_deletion(std::string_view conversation_id,
                                                   std::string_view selected_branches_json,
                                                   std::string_view selected_run_branches_json,
                                                   std::int64_t at);

    std::int64_t update_message_branch_selections(std::string_view conversation_id,
                                                 std::string_view selected_branches_json,
                                                 std::int64_t at);

    std::int64_t update_run_branch_selections(std::string_view conversation_id,
                                             std::string_view selected_run_branches_json,
                                             std::int64_t at);

    std::int64_t update_branch_selections(std::string_view conversation_id,
                                         std::string_view selected_branches_json,
                                         std::string_view selected_run_branches_json,
                                         std::int64_t at);

    std::optional<RunEntity> get_live_run(std::string_view conversation_id);

    std::int64_t next_run_sequence(std::string_view run_id);

    std::int64_t touch_run(std::string_view run_id, std::int64_t at);

    void delete_messages_by_ids(const std::vector<std::string>& ids);

    std::int64_t reparent_message_children(std::string_view removed_message_id,
                                          const std::optional<std::string>& replacement_parent_id);

    std::int64_t reparent_run_children(std::string_view removed_run_id,
                                      const std::optional<std::string>& replacement_parent_run_id);

    std::int64_t replace_context_compact_message_run(std::string_view message_id,
                                                    std::string_view old_run_id,
                                                    std::string_view new_run_id,
                                                    std::string_view model_name);

    // -- @Transaction bodies (transliterated) -------------------------------------------------

    /**
     * beginRecompactContextCompact: substitutes the terminal Compact's dedicated Run with a fresh
     * active Run while retaining the message identity and graph position.
     */
    MessageEntity begin_recompact_context_compact(const RunEntity& replacement_run,
                                                 std::string_view message_id,
                                                 std::string_view model_name,
                                                 std::string_view expected_selected_branches_json);

    /**
     * removeContextCompact: reparents the Compact's children, deletes its row/embeddings and
     * collapses both branch projections. `now_ms` is System.currentTimeMillis().
     */
    bool remove_context_compact(std::string_view message_id, std::int64_t now_ms);

private:
    SqlExecutor& sql_;
    TransactionStack stack_;
};

}  // namespace agora::core::context_compact
