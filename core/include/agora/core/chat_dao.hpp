#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

#include "agora/core/chat_automation_dao.hpp"
#include "agora/core/chat_entities.hpp"
#include "agora/core/chat_media_references_dao.hpp"
#include "agora/core/chat_provider_context_dao.hpp"
#include "agora/core/chat_search_dao.hpp"
#include "agora/core/context_compact.hpp"
#include "agora/core/new_chat_persist_dao.hpp"
#include "agora/core/run_branch_selection.hpp"
#include "agora/core/runtime_recovery.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::chat_dao {

using MessageEntity = ::agora::core::MessageEntity;
using ChatEntity = ::agora::core::ChatEntity;
using RunEntity = ::agora::core::context_compact::RunEntity;
using TaskEntity = ::agora::core::automation::TaskEntity;
using LoopEntity = ::agora::core::automation::LoopEntity;
using NewChatPersistEntity = ::agora::core::new_chat_persist::NewChatPersistEntity;
using SelectionMap = ::agora::core::context_compact::SelectionMap;

/** ChatConversation (model/ChatMessage.kt), the navigation projection. */
struct ChatConversation {
    std::string id;
    std::string title;
    std::optional<std::string> system_prompt_id;
    std::optional<std::string> model_id;
    std::optional<std::string> task_id;
    std::string origin;
    bool graduated = false;
    bool has_unread_generation = false;
    bool is_pinned = false;
    std::optional<std::string> selected_branches_json;
};

/** DrawerConversationRow (data/local/ChatDao.kt). */
struct DrawerConversationRow {
    std::string id;
    std::string title;
    bool has_unread_generation = false;
    bool is_pinned = false;
};

/** ConversationSettingsTransferEntity (ChatEntities.kt). */
struct ConversationSettingsTransferEntity {
    std::string conversation_id;
    std::optional<std::string> settings_json;
};

/** ConversationSettingsImportTransferEntity (ChatEntities.kt). */
struct ConversationSettingsImportTransferEntity {
    int id = 0;
    std::string transfer_id;
    std::string settings_json;
    std::string mode;

    /** The data-class init block: id == 0, non-blank transferId, MERGE/REPLACE. */
    void validate() const;
};

/** EmbeddingEntity (ChatEntities.kt): @PrimaryKey(autoGenerate = true). */
struct EmbeddingEntity {
    std::int64_t id = 0;
    std::string message_id;
    std::string model_id;
    std::vector<std::uint8_t> embedding;
    std::string chunk_text;
    int dimension = 0;
    std::optional<std::string> source_fingerprint;
};

/** MessageStreamCheckpoint (ChatEntities.kt), the @Update projection over MessageEntity. */
struct MessageStreamCheckpoint {
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

/** RunGraphCommit (ChatEntities.kt). */
struct RunGraphCommit {
    std::vector<MessageEntity> messages;
    SelectionMap message_selections;
    SelectionMap run_selections;
};

/** ToolRoundCommit (ChatEntities.kt). */
struct ToolRoundCommit {
    std::vector<MessageEntity> messages;
    bool inserted = false;
};

/** ExecutionMessageSummaryRow (MessageContextTopology.kt). */
struct ExecutionMessageSummaryRow {
    std::string id;
    std::string conversation_id;
    run_recovery::Participant participant = run_recovery::Participant::model;
    run_recovery::MessageStatus status = run_recovery::MessageStatus::success;
    std::int64_t timestamp = 0;
    std::string preview;
};

/**
 * Port of data/local/ChatDao.kt, the single Room DAO inherited by the app's repositories.
 *
 * The interface composes the six already-ported DAO surfaces by public inheritance, so callers see
 * the same member set as Kotlin. ChatDao's own SQL text and every @Transaction body are
 * transliterated below. Observed queries expose their current rows as the Flow's initial emission;
 * invalidation-driven re-emission remains the caller's store concern.
 *
 * Clock reads inside the transaction bodies are parameters (`now_ms`) so golden traces stay
 * deterministic. The Room @Transaction nesting is one TransactionStack per concrete DAO; this
 * module's own stack wraps only its own bodies, exactly as the Kotlin default methods do.
 */
class ChatDao : public automation::ChatAutomationDao,
                public context_compact::ContextCompactDao,
                public provider_context::ChatProviderContextDao,
                public chat_search::ChatSearchDao,
                public media_references::ChatMediaReferencesDao,
                public new_chat_persist::NewChatPersistDao {
public:
    explicit ChatDao(SqlExecutor& sql);

    // -- reads / projections -----------------------------------------------------------------

    std::vector<ChatConversation> get_all_conversations();

    std::vector<DrawerConversationRow> observe_drawer_conversations(int limit);

    std::vector<DrawerConversationRow> get_drawer_conversations(const std::vector<std::string>& ids);

    std::vector<ChatEntity> get_executions_for_task(std::string_view task_id);

    std::optional<ChatEntity> observe_conversation(std::string_view conversation_id);

    std::optional<ConversationSettingsTransferEntity> get_conversation_settings_transfer(
        std::string_view conversation_id);

    std::vector<ConversationSettingsTransferEntity> get_pending_conversation_settings_transfers();

    void upsert_conversation_settings_transfer(const ConversationSettingsTransferEntity& entity);

    int delete_conversation_settings_transfer(std::string_view conversation_id);

    std::optional<ConversationSettingsImportTransferEntity>
    get_conversation_settings_import_transfer();

    void upsert_conversation_settings_import_transfer(
        const ConversationSettingsImportTransferEntity& entity);

    int delete_conversation_settings_import_transfer(std::string_view transfer_id);

    std::optional<MessageEntity> observe_message(std::string_view message_id);

    int stop_stuck_messages_for_conversation(std::string_view conversation_id);

    // -- conversation writes -----------------------------------------------------------------

    void upsert_conversation(const ChatEntity& conversation);

    int update_conversation_model(std::string_view conversation_id,
                                  const std::optional<std::string>& model_id,
                                  std::int64_t at);

    int update_conversation_system_prompt(std::string_view conversation_id,
                                          const std::optional<std::string>& prompt_id,
                                          std::int64_t at);

    int touch_conversation_data(std::string_view conversation_id, std::int64_t at);

    void upsert_loop(const LoopEntity& loop, std::int64_t now_ms);

    int deactivate_loop_if_unchanged(std::string_view conversation_id,
                                     std::int64_t expected_revision,
                                     int expected_cycle_count,
                                     std::int64_t expected_interval_ms,
                                     std::int64_t expected_next_fire_at,
                                     int normalized_max_cycles,
                                     std::int64_t now_ms);

    void delete_loop(std::string_view conversation_id, std::int64_t now_ms);

    int update_conversation_title(std::string_view conversation_id, std::string_view title,
                                  std::int64_t at);

    int set_conversation_pinned(std::string_view conversation_id, bool pinned, std::int64_t at);

    int set_conversation_unread_generation(std::string_view conversation_id, bool unread);

    int replace_conversation_model_references(std::string_view old_model_id,
                                              const std::optional<std::string>& new_model_id,
                                              std::int64_t at);

    int replace_new_chat_model_reference(std::string_view old_model_id,
                                         const std::optional<std::string>& new_model_id);

    int update_conversation_title_if_unchanged(std::string_view conversation_id,
                                               std::string_view expected_title,
                                               std::string_view new_title,
                                               std::int64_t at);

    // -- message / run writes ----------------------------------------------------------------

    void upsert_message(const MessageEntity& message);

    void upsert_run(const RunEntity& run);

    std::vector<RunEntity> get_runs_for_conversation(std::string_view conversation_id);

    std::vector<RunEntity> get_runs_for_conversation_snapshot(std::string_view conversation_id);

    int update_conversation_for_run_admission(std::string_view conversation_id,
                                              std::string_view selected_branches_json,
                                              std::string_view selected_run_branches_json,
                                              const std::optional<std::string>& model_id,
                                              std::int64_t at,
                                              bool touch_conversation_on_admission);

    bool delete_message_subtree(std::string_view conversation_id,
                                std::string_view root_message_id,
                                const std::vector<std::string>& stale_message_ids,
                                const std::vector<std::string>& root_run_ids_to_delete,
                                std::string_view selected_branches_json,
                                std::string_view selected_run_branches_json,
                                std::int64_t at);

    RunGraphCommit create_run_with_messages(const RunEntity& run,
                                            const std::vector<MessageEntity>& messages,
                                            const SelectionMap& message_selection_updates,
                                            const std::optional<std::string>& conversation_model_id,
                                            std::int64_t at,
                                            bool touch_conversation_on_admission);

    RunGraphCommit create_conversation_run_with_messages(
        const ChatEntity& conversation,
        const RunEntity& run,
        const std::vector<MessageEntity>& messages,
        const SelectionMap& message_selection_updates,
        std::string_view conversation_model_id,
        const std::optional<std::string>& conversation_settings_json,
        const std::optional<NewChatPersistEntity>& expected_new_chat_persist,
        std::int64_t at);

    void import_run_graph(const std::vector<RunEntity>& runs,
                          const std::vector<MessageEntity>& messages);

    void replace_imported_conversation_graph(const std::vector<ChatEntity>& conversations,
                                             const std::vector<RunEntity>& runs,
                                             const std::vector<MessageEntity>& messages);

    void create_fork_graph(const ChatEntity& conversation,
                           const std::vector<RunEntity>& runs,
                           const std::vector<MessageEntity>& messages,
                           const std::vector<std::pair<std::string, std::string>>&
                               source_to_fork_message_ids);

    ToolRoundCommit append_tool_round_to_run(const std::vector<MessageEntity>& messages,
                                             int expected_pass,
                                             std::int64_t now_ms);

    int mark_run_stopping(std::string_view run_id, std::int64_t at);

    int terminalize_live_run(std::string_view run_id,
                             conversation_runtime::RunStatus status,
                             conversation_runtime::RunEndReason reason,
                             std::int64_t at);

    int stop_in_flight_model_messages(std::string_view run_id);

    bool finish_generation(const MessageStreamCheckpoint& checkpoint,
                           std::string_view conversation_id,
                           std::string_view run_id,
                           conversation_runtime::RunStatus status,
                           conversation_runtime::RunEndReason reason,
                           std::int64_t at,
                           bool mark_conversation_unread);

    bool finish_stopped_generation(const std::vector<MessageStreamCheckpoint>& checkpoints,
                                   std::string_view conversation_id,
                                   const std::optional<std::string>& run_id,
                                   std::int64_t at);

    int recover_conversation_runtime(std::string_view conversation_id, std::int64_t at);

    bool update_conversation_message_checkpoint(std::string_view conversation_id,
                                                const MessageStreamCheckpoint& checkpoint,
                                                std::int64_t at);

    int update_message_checkpoint(const MessageStreamCheckpoint& checkpoint);

    // -- deletes ------------------------------------------------------------------------------

    void delete_conversation(std::string_view conversation_id);

    void delete_messages_by_conversation(std::string_view conversation_id);

    int update_run_parent(std::string_view run_id,
                          const std::optional<std::string>& new_parent_run_id);

    void delete_embeddings_by_conversation(std::string_view conversation_id);

    void delete_orphaned_embeddings();

    std::optional<MessageEntity> get_last_message_for_conversation(
        std::string_view conversation_id);

    std::vector<ExecutionMessageSummaryRow> observe_execution_messages_for_task(
        std::string_view task_id);

    // -- embeddings ---------------------------------------------------------------------------

    std::vector<std::int64_t> insert_embeddings(const std::vector<EmbeddingEntity>& embeddings);

    std::vector<EmbeddingEntity> get_embeddings_by_message_ids(
        const std::vector<std::string>& message_ids);

    std::optional<EmbeddingEntity> get_embedding(std::string_view message_id);

    std::vector<EmbeddingEntity> get_all_embeddings();

    // -- bulk export / import -----------------------------------------------------------------

    std::vector<MessageEntity> get_messages_by_ids(const std::vector<std::string>& ids);

    void update_draft(std::string_view id,
                      std::string_view text,
                      const std::optional<std::string>& attachments,
                      std::int64_t at);

    std::vector<ChatEntity> get_all_conversations_list();

    int compare_and_set_run_branch_selections(std::string_view conversation_id,
                                              std::string_view expected,
                                              std::string_view replacement);

    std::vector<std::string> get_all_conversation_ids();

    std::vector<std::string> get_all_task_ids();

    std::vector<MessageEntity> get_messages_page(const std::optional<std::string>& after_id,
                                                 int limit);

    std::vector<MessageEntity> get_conversation_messages_page(
        std::string_view conversation_id,
        const std::optional<std::string>& after_id,
        int limit);

    void delete_all_conversations();

    std::vector<std::string> find_existing_message_ids(const std::vector<std::string>& ids);

    // -- configured model/provider rewrites ----------------------------------------------------

    void replace_configured_model_references(std::string_view old_model_id,
                                             const std::optional<std::string>& new_model_id,
                                             std::int64_t now_ms);

    int rename_conversation_provider_model_references(std::string_view old_provider,
                                                      std::string_view new_provider,
                                                      std::int64_t at);

    int touch_conversations_with_message_provider(std::string_view old_provider,
                                                  std::int64_t at);

    int rename_new_chat_provider_model_reference(std::string_view old_provider,
                                                 std::string_view new_provider);

    int rename_message_provider_model_references(std::string_view old_provider,
                                                 std::string_view new_provider);

    void rename_configured_provider_model_references(std::string_view old_provider,
                                                     std::string_view new_provider,
                                                     std::int64_t now_ms);

private:
    template <typename Fn>
    auto in_transaction(Fn&& body) -> decltype(body()) {
        stack_.begin();
        try {
            auto result = body();
            stack_.mark_successful();
            stack_.end();
            return result;
        } catch (...) {
            stack_.end();
            throw;
        }
    }

    RunGraphCommit create_run_with_messages_body(
        const RunEntity& run,
        const std::vector<MessageEntity>& messages,
        const SelectionMap& message_selection_updates,
        const std::optional<std::string>& conversation_model_id,
        std::int64_t at,
        bool touch_conversation_on_admission);

    void import_run_graph_body(const std::vector<RunEntity>& runs,
                               const std::vector<MessageEntity>& messages);

    SqlExecutor& sql_;
    TransactionStack stack_;
};

}  // namespace agora::core::chat_dao
