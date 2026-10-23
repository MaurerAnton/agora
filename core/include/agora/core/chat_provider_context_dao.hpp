#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <vector>

#include "agora/core/run_recovery.hpp"
#include "agora/core/sql_boundary.hpp"

namespace agora::core::provider_context {

/** MessageContextTopology (data/local/MessageContextTopology.kt): one payload-free message row. */
struct MessageContextTopology {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_id;
    run_recovery::MessageStatus status = run_recovery::MessageStatus::success;
    run_recovery::Participant participant = run_recovery::Participant::model;
    std::int64_t timestamp = 0;
    int token_count = 0;
    std::optional<std::string> model_name;
    std::string run_id;
    std::int64_t run_sequence = 0;
    std::optional<int> consumed_at_pass;
};

/** ConversationProviderContextState: the nullable conversation selection column. */
struct ConversationProviderContextState {
    std::optional<std::string> selected_branches_json;
};

/** ProviderContextTopologySnapshot: the state and topology read in one transaction. */
struct ProviderContextTopologySnapshot {
    std::optional<std::string> selected_branches_json;
    std::vector<MessageContextTopology> messages;
};

/**
 * Port of data/local/ChatProviderContextDao.kt: the payload-free context reads inherited by
 * ChatDao. The two topology members share one SQL text; the transaction member reads the
 * conversation state first and short-circuits to null when the conversation is absent, exactly
 * like the Kotlin default-method body.
 *
 * The Kotlin `observeMessageContextTopology` returns a Room Flow. The C++ application has no
 * coroutine stream, so the port exposes the query's current rows, which is the Flow's initial
 * emission; invalidation-driven re-emission is Room/Database platform behavior and is owned by
 * the caller's store, not by this DAO.
 */
class ChatProviderContextDao {
public:
    explicit ChatProviderContextDao(SqlExecutor& sql) : sql_(sql), stack_(sql) {}

    std::optional<ConversationProviderContextState> get_provider_context_state(
        std::string_view conversation_id);

    std::vector<MessageContextTopology> get_message_context_topology(
        std::string_view conversation_id);

    std::vector<MessageContextTopology> observe_message_context_topology(
        std::string_view conversation_id);

    std::optional<ProviderContextTopologySnapshot> get_provider_context_topology_snapshot(
        std::string_view conversation_id);

private:
    SqlExecutor& sql_;
    TransactionStack stack_;
};

}  // namespace agora::core::provider_context
