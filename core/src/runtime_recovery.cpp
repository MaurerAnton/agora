// Port of data/local/ChatDaoRuntimeRecovery.kt (the @Transaction body of
// ChatDao.recoverConversationRuntime).
//
// Every step is the Kotlin body in order: the reducer fold, the per-message checkpoint rewrite,
// the durable terminalization, the branch projection repair, the stuck-row sweep, and the single
// watermark touch that must succeed whenever anything changed. The reducer, the message policy
// and the branch validation are the already-ported conversation_runtime, run_recovery and
// run_branch_selection modules; this file is the orchestration between them.
#include "agora/core/runtime_recovery.hpp"

#include <cstdint>
#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::runtime_recovery {

namespace {

namespace conversation = agora::core::conversation_runtime;
namespace run_branch = agora::core::run_branch_selection;
namespace recovery = agora::core::run_recovery;

/** withTransaction: the block marks the level successful unless it throws. */
template <typename Body>
auto run_in_transaction(RecoveryStore& store, Body&& body) -> decltype(body()) {
    store.begin_transaction();
    try {
        auto result = body();
        store.set_transaction_successful();
        store.end_transaction();
        return result;
    } catch (...) {
        store.end_transaction();
        throw;
    }
}

/** Kotlin's `list.filterIsInstance<RecoverDurableRun>().single()`. */
const conversation::Effect& single_recover_effect(const conversation::Transition& requested) {
    const conversation::Effect* found = nullptr;
    int matches = 0;
    for (const conversation::Effect& effect : requested.effects) {
        if (effect.kind == conversation::Effect::Kind::RecoverDurableRun) {
            found = &effect;
            ++matches;
        }
    }
    if (matches == 0) {
        throw KotlinError("NoSuchElementException", "Empty list contains no element.");
    }
    if (matches > 1) {
        throw KotlinError("NoSuchElementException", "List contains more than one element.");
    }
    return *found;
}

/** The checkpoint projection the body builds from the row it read. */
MessageCheckpoint checkpoint_of(const RecoveryMessage& message,
                                recovery::MessageStatus recovered_status,
                                const std::optional<std::string>& recovered_tool_json) {
    MessageCheckpoint checkpoint;
    checkpoint.id = message.id;
    checkpoint.text = message.text;
    checkpoint.images = message.images;
    checkpoint.thoughts = message.thoughts;
    checkpoint.thought_title = message.thought_title;
    checkpoint.token_count = message.token_count;
    checkpoint.input_token_count = message.input_token_count;
    checkpoint.cached_input_token_count = message.cached_input_token_count;
    checkpoint.cache_write_input_token_count = message.cache_write_input_token_count;
    checkpoint.uncached_input_token_count = message.uncached_input_token_count;
    checkpoint.output_token_count = message.output_token_count;
    checkpoint.reasoning_token_count = message.reasoning_token_count;
    checkpoint.generation_duration_ms = message.generation_duration_ms;
    checkpoint.status = recovered_status;
    checkpoint.thought_time_ms = message.thought_time_ms;
    checkpoint.tool_call_json = recovered_tool_json;
    return checkpoint;
}

}  // namespace

std::int64_t execute_runtime_recovery(RecoveryStore& store, std::string_view conversation_id,
                                      std::int64_t at) {
    return run_in_transaction(store, [&] {
        const std::optional<ConversationSnapshot> conversation =
            store.get_conversation(conversation_id);
        if (!conversation.has_value()) return std::int64_t{0};
        std::int64_t changed_rows = 0;

        const std::optional<RecoveryRun> live_run = store.get_live_run(conversation_id);
        if (live_run.has_value()) {
            conversation::RunRecoverySnapshot snapshot;
            snapshot.conversation_id = std::string(conversation_id);
            snapshot.run_id = live_run->id;
            snapshot.pass = live_run->current_pass;
            snapshot.status = live_run->status;

            conversation::RunState idle;
            idle.kind = conversation::RunState::Kind::Idle;
            idle.conversation_id = std::string(conversation_id);
            conversation::Command recover;
            recover.kind = conversation::Command::Kind::Recover;
            recover.snapshot = snapshot;
            const conversation::Transition requested = conversation::reduce(idle, recover);

            const conversation::Effect& recovery_effect = single_recover_effect(requested);
            kotlin_check(recovery_effect.prior_status == live_run->status);

            for (const RecoveryMessage& message :
                 store.get_messages_for_runs({live_run->id})) {
                const recovery::MessageStatus recovered_status =
                    recovery::recover_message_status(message.participant, message.status);
                std::optional<std::string> recovered_tool_json;
                if (message.tool_call_json.has_value()) {
                    // `toolCallJson?.let { raw -> stopIncompleteToolsJson(raw) ?: raw }`
                    recovered_tool_json =
                        recovery::stop_incomplete_tools_json(*message.tool_call_json);
                    if (!recovered_tool_json.has_value()) recovered_tool_json = *message.tool_call_json;
                }
                if (recovered_status != message.status ||
                    recovered_tool_json != message.tool_call_json) {
                    changed_rows += store.update_message_checkpoint(
                        checkpoint_of(message, recovered_status, recovered_tool_json));
                }
            }

            const bool durable_success =
                store.terminalize_live_run(recovery_effect.identity.run_id,
                                           conversation::RunStatus::Stopped,
                                           conversation::RunEndReason::ProcessRecovered, at) == 1;

            conversation::Command completed;
            completed.kind = conversation::Command::Kind::RecoveryCompleted;
            completed.identity = recovery_effect.identity;
            completed.success = durable_success;
            const conversation::Transition transition =
                conversation::reduce(requested.state, completed);
            kotlin_check(durable_success && transition.accepted() &&
                             transition.state.kind == conversation::RunState::Kind::Idle,
                         "Run recovery transaction lost ownership for " + live_run->id);
            changed_rows += 1;
        }

        if (conversation->selected_run_branches_json.has_value()) {
            const std::string& raw = *conversation->selected_run_branches_json;
            // runCatching { Json.decodeFromString<Map<String, String>>(raw) }.getOrNull()
            const std::optional<run_branch::SelectionMap> decoded =
                run_branch::decode_selection_map(raw);
            if (decoded.has_value()) {
                std::vector<run_branch::Selection> selections;
                selections.reserve(decoded->size());
                for (const auto& [parent, child] : *decoded) {
                    selections.push_back(run_branch::Selection{parent, child});
                }
                run_branch::SelectionMap repaired;
                for (const run_branch::Selection& kept : run_branch::retain_valid_edges(
                         selections, store.get_runs_for_conversation_snapshot(conversation_id))) {
                    repaired.emplace_back(kept.parent_run_id, kept.child_run_id);
                }
                if (repaired != *decoded) {
                    changed_rows += store.compare_and_set_run_branch_selections(
                        conversation_id, raw, run_branch::encode_selection_map(repaired));
                }
            }
        }

        changed_rows += store.stop_stuck_messages_for_conversation(conversation_id);
        if (changed_rows > 0) {
            kotlin_check(store.touch_conversation_data(conversation_id, at) == 1);
        }
        return changed_rows;
    });
}

}  // namespace agora::core::runtime_recovery
