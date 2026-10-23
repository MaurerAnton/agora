// Port of data/local/ChatProviderContextDao.kt.
//
// Both topology members run the same SELECT (timestamp ASC, id ASC) and differ only in their
// Kotlin return shape: a suspend List and a Flow<List>. The C++ port exposes the list for both;
// see the header for the Flow boundary. The snapshot member is the @Transaction default-method
// body: state first, null short-circuit, then messages, wrapped in Room's transaction nesting.
#include "agora/core/chat_provider_context_dao.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::provider_context {

namespace {

namespace recovery = agora::core::run_recovery;

recovery::MessageStatus message_status_from_wire(const std::string& wire) {
    if (wire == "TRANSCRIBING") return recovery::MessageStatus::transcribing;
    if (wire == "SENDING") return recovery::MessageStatus::sending;
    if (wire == "THINKING") return recovery::MessageStatus::thinking;
    if (wire == "TOOL_CALLING") return recovery::MessageStatus::tool_calling;
    if (wire == "SUCCESS") return recovery::MessageStatus::success;
    if (wire == "STOPPED") return recovery::MessageStatus::stopped;
    if (wire == "ERROR") return recovery::MessageStatus::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.MessageStatus." + wire);
}

recovery::Participant participant_from_wire(const std::string& wire) {
    if (wire == "USER") return recovery::Participant::user;
    if (wire == "MODEL") return recovery::Participant::model;
    if (wire == "ERROR") return recovery::Participant::error;
    throw KotlinError("IllegalArgumentException",
                      "No enum constant com.newoether.agora.model.Participant." + wire);
}

constexpr const char* kTopologySql =
    "SELECT id, conversationId, parentId, status, participant, timestamp, tokenCount, modelName, "
    "runId, runSequence, consumedAtPass FROM messages WHERE conversationId = ? "
    "ORDER BY timestamp ASC, id ASC";

std::vector<MessageContextTopology> read_topology(SqlExecutor& sql,
                                                  std::string_view conversation_id) {
    const std::vector<SqlRow> rows =
        sql.query(kTopologySql, {SqlValue::text_value(std::string(conversation_id))});
    std::vector<MessageContextTopology> out;
    out.reserve(rows.size());
    for (const SqlRow& row : rows) {
        kotlin_check(row.size() == 11, "MessageContextTopology: row shape mismatch");
        MessageContextTopology topology;
        topology.id = row[0].as_text();
        topology.conversation_id = row[1].as_text();
        topology.parent_id = row[2].text_or_null();
        topology.status = message_status_from_wire(row[3].as_text());
        topology.participant = participant_from_wire(row[4].as_text());
        topology.timestamp = row[5].as_integer();
        topology.token_count = static_cast<int>(row[6].as_integer());
        topology.model_name = row[7].text_or_null();
        topology.run_id = row[8].as_text();
        topology.run_sequence = row[9].as_integer();
        const std::optional<std::int64_t> consumed = row[10].integer_or_null();
        topology.consumed_at_pass =
            consumed.has_value() ? std::optional<int>(static_cast<int>(*consumed)) : std::nullopt;
        out.push_back(std::move(topology));
    }
    return out;
}

}  // namespace

std::optional<ConversationProviderContextState>
ChatProviderContextDao::get_provider_context_state(std::string_view conversation_id) {
    const std::vector<SqlRow> rows = sql_.query(
        "SELECT selectedBranchesJson FROM conversations WHERE id = ?",
        {SqlValue::text_value(std::string(conversation_id))});
    if (rows.empty()) return std::nullopt;
    kotlin_check(rows.front().size() == 1, "ConversationProviderContextState: row shape mismatch");
    ConversationProviderContextState state;
    state.selected_branches_json = rows.front().front().text_or_null();
    return state;
}

std::vector<MessageContextTopology> ChatProviderContextDao::get_message_context_topology(
    std::string_view conversation_id) {
    return read_topology(sql_, conversation_id);
}

std::vector<MessageContextTopology> ChatProviderContextDao::observe_message_context_topology(
    std::string_view conversation_id) {
    return read_topology(sql_, conversation_id);
}

std::optional<ProviderContextTopologySnapshot>
ChatProviderContextDao::get_provider_context_topology_snapshot(
    std::string_view conversation_id) {
    stack_.begin();
    try {
        std::optional<ProviderContextTopologySnapshot> result;
        const std::optional<ConversationProviderContextState> state =
            get_provider_context_state(conversation_id);
        if (state.has_value()) {
            ProviderContextTopologySnapshot snapshot;
            snapshot.selected_branches_json = state->selected_branches_json;
            snapshot.messages = get_message_context_topology(conversation_id);
            result = std::move(snapshot);
        }
        stack_.mark_successful();
        stack_.end();
        return result;
    } catch (...) {
        stack_.end();
        throw;
    }
}

}  // namespace agora::core::provider_context
