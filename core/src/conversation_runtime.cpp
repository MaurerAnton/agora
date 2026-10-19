#include "agora/core/conversation_runtime.hpp"

#include <limits>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::conversation_runtime {
namespace {

bool same_owner(const RuntimeRunIdentity& first, const RuntimeRunIdentity& second) {
    return first.conversation_id == second.conversation_id &&
           first.owner_token == second.owner_token;
}

RuntimeRunIdentity run_identity(const RunEffectIdentity& identity) {
    return {identity.conversation_id, identity.owner_token, identity.run_id, identity.pass};
}

RunEffectIdentity effect_identity(const RuntimeRunIdentity& identity, const std::string& effect_id) {
    return {identity.conversation_id, identity.owner_token, identity.run_id.value_or(std::string()),
            identity.pass, effect_id};
}

RunEffectIdentity derived(const RunEffectIdentity& identity, std::string_view prefix) {
    return {identity.conversation_id, identity.owner_token, identity.run_id, identity.pass,
            std::string(prefix) + "-" + identity.effect_id};
}

Transition reject(const RunState& state, CommandRejection rejection) {
    Transition transition;
    transition.state = state;
    transition.rejection = rejection;
    return transition;
}

Transition accepted(RunState state, std::vector<Effect> effects) {
    Transition transition;
    transition.state = std::move(state);
    transition.effects = std::move(effects);
    return transition;
}

RunState idle(std::string conversation_id) {
    RunState state;
    state.kind = RunState::Kind::Idle;
    state.conversation_id = std::move(conversation_id);
    return state;
}

Effect effect_with_identity(Effect::Kind kind, RunEffectIdentity identity) {
    Effect effect;
    effect.kind = kind;
    effect.identity = std::move(identity);
    return effect;
}

Effect release_slot(RuntimeRunIdentity identity, SlotReleaseReason reason) {
    Effect effect;
    effect.kind = Effect::Kind::ReleaseSlot;
    effect.runtime_identity = std::move(identity);
    effect.release_reason = reason;
    return effect;
}

const std::string& command_conversation_id(const Command& command) {
    switch (command.kind) {
        case Command::Kind::Recover:
            return command.snapshot.conversation_id;
        case Command::Kind::AcquireSlot:
        case Command::Kind::BindRun:
        case Command::Kind::StopRequested:
        case Command::Kind::CoroutineSettled:
            return command.runtime_identity.conversation_id;
        default:
            return command.identity.conversation_id;
    }
}

// ---- commands -------------------------------------------------------------------------------

Transition request_recovery(const RunState& state, const Command& command) {
    switch (state.kind) {
        case RunState::Kind::Idle: {
            const RunRecoverySnapshot& snapshot = command.snapshot;
            RunEffectIdentity identity{snapshot.conversation_id,
                                       std::numeric_limits<std::int64_t>::max(),
                                       snapshot.run_id,
                                       snapshot.pass,
                                       "recover-" + snapshot.run_id + "-" +
                                           std::to_string(snapshot.pass)};
            RunState next;
            next.kind = RunState::Kind::Recovering;
            next.conversation_id = snapshot.conversation_id;
            next.snapshot = snapshot;
            next.effect_identity = identity;
            Effect effect;
            effect.kind = Effect::Kind::RecoverDurableRun;
            effect.identity = identity;
            effect.prior_status = snapshot.status;
            return accepted(std::move(next), {std::move(effect)});
        }
        case RunState::Kind::Recovering:
            return reject(state, state.snapshot == command.snapshot ? CommandRejection::DuplicateResult
                                                                    : CommandRejection::StaleIdentity);
        default:
            return reject(state, CommandRejection::IllegalState);
    }
}

Transition complete_recovery(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Recovering) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.effect_identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (!command.success) {
        if (state.failure_reported) return reject(state, CommandRejection::DuplicateResult);
        RunState next = state;
        next.failure_reported = true;
        return accepted(std::move(next),
                        {effect_with_identity(Effect::Kind::RunRecoveryFailed, command.identity)});
    }
    return accepted(idle(state.conversation_id), {});
}

Transition acquire(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Idle) return reject(state, CommandRejection::IllegalState);
    RunState next;
    next.kind = RunState::Kind::Active;
    next.conversation_id = command.runtime_identity.conversation_id;
    next.identity = command.runtime_identity;
    Effect effect;
    effect.kind = Effect::Kind::SlotActivated;
    effect.runtime_identity = command.runtime_identity;
    return accepted(std::move(next), {std::move(effect)});
}

Transition busy(const RunState& state, const Command& command) {
    return accepted(state, {effect_with_identity(Effect::Kind::RejectSendBusy, command.identity)});
}

Transition deferred(const RunState& state, const Command& command) {
    return accepted(state, {effect_with_identity(Effect::Kind::AwaitRunRelease, command.identity)});
}

Transition request_send(const RunState& state, const Command& command) {
    switch (state.kind) {
        case RunState::Kind::Idle:
            if (command.direct_only && command.has_pending_guidance) {
                return accepted(state,
                                {effect_with_identity(Effect::Kind::RejectSendBusy, command.identity)});
            }
            if (command.has_pending_guidance) {
                return accepted(
                    state,
                    {effect_with_identity(Effect::Kind::DrainGuidanceFirst, command.identity)});
            }
            {
                RunState next;
                next.kind = RunState::Kind::Preparing;
                next.conversation_id = command.identity.conversation_id;
                next.owner_identity = {command.identity.conversation_id, command.identity.owner_token,
                                       std::nullopt, 0};
                next.input_effect_identity = command.identity;
                return accepted(
                    std::move(next),
                    {effect_with_identity(Effect::Kind::PersistAcceptedInput, command.identity)});
            }
        case RunState::Kind::Preparing:
            if (command.direct_only) return busy(state, command);
            {
                RunEffectIdentity identity = command.identity;
                identity.owner_token = state.owner_identity.owner_token;
                identity.run_id = state.input_effect_identity.run_id;
                identity.pass = 0;
                return accepted(
                    state,
                    {effect_with_identity(Effect::Kind::AcceptGuidance, std::move(identity))});
            }
        case RunState::Kind::Active:
            if (command.direct_only) return busy(state, command);
            if (!state.identity.run_id.has_value()) return deferred(state, command);
            {
                RunEffectIdentity identity{state.identity.conversation_id,
                                           state.identity.owner_token,
                                           *state.identity.run_id,
                                           state.identity.pass,
                                           command.identity.effect_id};
                return accepted(
                    state,
                    {effect_with_identity(Effect::Kind::AcceptGuidance, std::move(identity))});
            }
        case RunState::Kind::Stopping:
        case RunState::Kind::Finalizing:
            return command.direct_only ? busy(state, command) : deferred(state, command);
        case RunState::Kind::Recovering:
            return reject(state, CommandRejection::IllegalState);
    }
    return reject(state, CommandRejection::IllegalState);
}

Transition bind_durable_run_after_stop(const RunState& stopping,
                                       const RuntimeRunIdentity& bound_identity) {
    if (stopping.identity.run_id.has_value()) {
        return reject(stopping, stopping.identity == bound_identity
                                    ? CommandRejection::DuplicateResult
                                    : CommandRejection::StaleIdentity);
    }
    if (stopping.finalization_effect_id.has_value() || !stopping.persistence_settled) {
        return reject(stopping, CommandRejection::IllegalState);
    }
    if (!same_owner(stopping.identity, bound_identity)) {
        return reject(stopping, CommandRejection::StaleIdentity);
    }
    const std::string effect_id = "stop-" + std::to_string(bound_identity.owner_token);
    const RunEffectIdentity identity = effect_identity(bound_identity, effect_id);
    RunState next = stopping;
    next.identity = bound_identity;
    next.finalization_effect_id = effect_id;
    next.persistence_settled = false;
    return accepted(std::move(next),
                    {effect_with_identity(Effect::Kind::FinalizeStop, identity)});
}

Transition input_persisted(const RunState& state, const Command& command) {
    switch (state.kind) {
        case RunState::Kind::Preparing:
            if (state.input_effect_identity != command.identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            {
                RunState next;
                next.kind = RunState::Kind::Active;
                next.conversation_id = command.identity.conversation_id;
                next.identity = run_identity(command.identity);
                return accepted(std::move(next), {});
            }
        case RunState::Kind::Stopping:
            return bind_durable_run_after_stop(state, run_identity(command.identity));
        default:
            return reject(state, CommandRejection::IllegalState);
    }
}

Transition input_persistence_failed(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Preparing) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.input_effect_identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (state.input_failure_reported) return reject(state, CommandRejection::DuplicateResult);
    RunState next = state;
    next.input_failure_reported = true;
    return accepted(std::move(next), {});
}

Transition abandon_send_launch(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Preparing) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.input_effect_identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    return accepted(idle(state.conversation_id),
                    {release_slot(state.owner_identity, SlotReleaseReason::SendLaunchAbandoned)});
}

Transition bind_run(const RunState& state, const Command& command) {
    if (state.kind == RunState::Kind::Stopping) {
        return bind_durable_run_after_stop(state, command.runtime_identity);
    }
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled || state.provider_phase.kind != ProviderPhase::Kind::None ||
        state.tool_phase.kind != ToolPhase::Kind::None) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (!same_owner(state.identity, command.runtime_identity)) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    const std::optional<std::string>& current_run_id = state.identity.run_id;
    if (current_run_id.has_value() && *current_run_id != command.runtime_identity.run_id) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (command.runtime_identity.pass < state.identity.pass) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (state.identity == command.runtime_identity) {
        return reject(state, CommandRejection::DuplicateResult);
    }
    if (current_run_id.has_value() &&
        command.runtime_identity.pass != state.identity.pass + 1) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    RunState next;
    next.kind = RunState::Kind::Active;
    next.conversation_id = command.runtime_identity.conversation_id;
    next.identity = command.runtime_identity;
    return accepted(std::move(next), {});
}

Transition request_provider_pass(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled) return reject(state, CommandRejection::IllegalState);
    if (state.identity != run_identity(command.identity)) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (state.tool_phase.kind != ToolPhase::Kind::None) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.provider_phase.kind == ProviderPhase::Kind::None) {
        RunState next = state;
        next.provider_phase.kind = ProviderPhase::Kind::Running;
        next.provider_phase.identity = command.identity;
        return accepted(std::move(next),
                        {effect_with_identity(Effect::Kind::StartProviderPass, command.identity)});
    }
    return reject(state, state.provider_phase.identity == command.identity
                             ? CommandRejection::DuplicateResult
                             : CommandRejection::StaleIdentity);
}

Transition complete_provider_pass(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled) return reject(state, CommandRejection::IllegalState);
    if (state.provider_phase.kind != ProviderPhase::Kind::Running) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.provider_phase.identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    RunState next = state;
    next.provider_phase = ProviderPhase{};
    Effect effect;
    effect.kind = Effect::Kind::ProviderPassAccepted;
    effect.identity = command.identity;
    effect.result = command.result;
    return accepted(std::move(next), {std::move(effect)});
}

Transition request_tool_batch(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled || state.provider_phase.kind != ProviderPhase::Kind::None) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.identity != run_identity(command.identity)) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    const RunEffectIdentity batch_identity = derived(command.identity, "tool-batch");
    switch (state.tool_phase.kind) {
        case ToolPhase::Kind::None: {
            RunState next = state;
            next.tool_phase.kind = ToolPhase::Kind::Executing;
            next.tool_phase.batch_identity = batch_identity;
            return accepted(
                std::move(next),
                {effect_with_identity(Effect::Kind::ExecuteToolBatch, batch_identity)});
        }
        case ToolPhase::Kind::Executing:
        case ToolPhase::Kind::Committing:
            return reject(state, state.tool_phase.batch_identity == batch_identity
                                     ? CommandRejection::DuplicateResult
                                     : CommandRejection::StaleIdentity);
    }
    return reject(state, CommandRejection::IllegalState);
}

Transition complete_tool_batch(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled) return reject(state, CommandRejection::IllegalState);
    switch (state.tool_phase.kind) {
        case ToolPhase::Kind::None:
            return reject(state, CommandRejection::IllegalState);
        case ToolPhase::Kind::Executing: {
            if (state.tool_phase.batch_identity != command.identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            const RunEffectIdentity commit_identity = derived(command.identity, "tool-round");
            RunState next = state;
            next.tool_phase.kind = ToolPhase::Kind::Committing;
            next.tool_phase.batch_identity = command.identity;
            next.tool_phase.commit_identity = commit_identity;
            next.tool_phase.failure_reported = false;
            return accepted(
                std::move(next),
                {effect_with_identity(Effect::Kind::CommitToolRound, commit_identity)});
        }
        case ToolPhase::Kind::Committing:
            return reject(state, state.tool_phase.batch_identity == command.identity
                                     ? CommandRejection::DuplicateResult
                                     : CommandRejection::StaleIdentity);
    }
    return reject(state, CommandRejection::IllegalState);
}

Transition commit_tool_round(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.coroutine_settled) return reject(state, CommandRejection::IllegalState);
    if (state.tool_phase.kind != ToolPhase::Kind::Committing) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.tool_phase.commit_identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (!command.success) {
        if (state.tool_phase.failure_reported) {
            return reject(state, CommandRejection::DuplicateResult);
        }
        RunState next = state;
        next.tool_phase.failure_reported = true;
        return accepted(std::move(next),
                        {effect_with_identity(Effect::Kind::ToolRoundCommitFailed,
                                              command.identity)});
    }
    RunState next = state;
    next.tool_phase = ToolPhase{};
    return accepted(std::move(next),
                    {effect_with_identity(Effect::Kind::ContinueProviderPass, command.identity)});
}

Transition request_finalization(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Active) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.identity != run_identity(command.identity)) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    RunState next;
    next.kind = RunState::Kind::Finalizing;
    next.conversation_id = state.conversation_id;
    next.identity = state.identity;
    next.effect_identity = command.identity;
    next.status = command.status;
    next.reason = command.reason;
    next.mark_conversation_unread = command.mark_conversation_unread;
    next.coroutine_settled = state.coroutine_settled;
    next.persistence_settled = false;
    Effect effect;
    effect.kind = Effect::Kind::FinalizeRun;
    effect.identity = command.identity;
    effect.status = command.status;
    effect.reason = command.reason;
    effect.mark_conversation_unread = command.mark_conversation_unread;
    return accepted(std::move(next), {std::move(effect)});
}

Transition release_settled_finalization(const RunState& finalizing) {
    return accepted(idle(finalizing.conversation_id),
                    {release_slot(finalizing.identity,
                                  SlotReleaseReason::NormalFinalizationSettled)});
}

Transition complete_finalization(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Finalizing) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (state.effect_identity != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (state.persistence_settled) return reject(state, CommandRejection::DuplicateResult);
    if (!command.success) {
        if (state.persistence_failure_reported) {
            return reject(state, CommandRejection::DuplicateResult);
        }
        RunState next = state;
        next.persistence_failure_reported = true;
        return accepted(std::move(next),
                        {effect_with_identity(Effect::Kind::RunFinalizationFailed,
                                              command.identity)});
    }
    if (state.coroutine_settled) return release_settled_finalization(state);
    RunState next = state;
    next.persistence_settled = true;
    return accepted(std::move(next), {});
}

Transition release_settled_stop(const RunState& stopping) {
    return accepted(idle(stopping.conversation_id),
                    {release_slot(stopping.identity, SlotReleaseReason::StopBarriersSettled)});
}

Transition request_stop(const RunState& state, const Command& command) {
    RuntimeRunIdentity active_identity;
    switch (state.kind) {
        case RunState::Kind::Recovering:
            return reject(state, CommandRejection::IllegalState);
        case RunState::Kind::Preparing:
            active_identity = state.owner_identity;
            break;
        case RunState::Kind::Active:
            active_identity = state.identity;
            break;
        case RunState::Kind::Finalizing:
            if (!state.persistence_failure_reported) {
                return reject(state, CommandRejection::IllegalState);
            }
            active_identity = state.identity;
            break;
        case RunState::Kind::Stopping:
            if (state.identity != command.runtime_identity ||
                state.finalization_effect_id != command.effect_id) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            if (!state.persistence_failure_reported) {
                return reject(state, CommandRejection::DuplicateResult);
            }
            if (!state.finalization_effect_id.has_value()) {
                return reject(state, CommandRejection::IllegalState);
            }
            {
                RunState next = state;
                next.persistence_failure_reported = false;
                return accepted(
                    std::move(next),
                    {effect_with_identity(
                        Effect::Kind::FinalizeStop,
                        effect_identity(state.identity, *state.finalization_effect_id))});
            }
        case RunState::Kind::Idle:
            return reject(state, CommandRejection::IllegalState);
    }
    if (active_identity != command.runtime_identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }

    std::vector<Effect> effects;
    Effect cancel;
    cancel.kind = Effect::Kind::CancelProviderPass;
    cancel.runtime_identity = active_identity;
    effects.push_back(std::move(cancel));
    if (command.effect_id.has_value()) {
        effects.push_back(effect_with_identity(
            Effect::Kind::FinalizeStop,
            effect_identity(active_identity, *command.effect_id)));
    }
    const bool persistence_settled = !command.requires_persistence;
    if (command.coroutine_already_settled && persistence_settled) {
        effects.push_back(release_slot(active_identity, SlotReleaseReason::EmptyStop));
        return accepted(idle(active_identity.conversation_id), std::move(effects));
    }
    RunState next;
    next.kind = RunState::Kind::Stopping;
    next.conversation_id = active_identity.conversation_id;
    next.identity = active_identity;
    next.finalization_effect_id = command.effect_id;
    next.coroutine_settled = command.coroutine_already_settled;
    next.persistence_settled = persistence_settled;
    return accepted(std::move(next), std::move(effects));
}

Transition settle_coroutine(const RunState& state, const Command& command) {
    switch (state.kind) {
        case RunState::Kind::Idle:
        case RunState::Kind::Recovering:
            return reject(state, CommandRejection::IllegalState);
        case RunState::Kind::Preparing:
            if (state.owner_identity != command.runtime_identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            return accepted(
                idle(state.conversation_id),
                {release_slot(state.owner_identity, SlotReleaseReason::NormalCompletion)});
        case RunState::Kind::Active:
            if (state.identity != command.runtime_identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            if (state.coroutine_settled) return reject(state, CommandRejection::DuplicateResult);
            if (state.identity.run_id.has_value()) {
                RunState next = state;
                next.coroutine_settled = true;
                return accepted(std::move(next), {});
            }
            return accepted(idle(state.conversation_id),
                            {release_slot(state.identity, SlotReleaseReason::NormalCompletion)});
        case RunState::Kind::Finalizing:
            if (state.identity != command.runtime_identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            if (state.coroutine_settled) return reject(state, CommandRejection::DuplicateResult);
            if (state.persistence_settled) return release_settled_finalization(state);
            {
                RunState next = state;
                next.coroutine_settled = true;
                return accepted(std::move(next), {});
            }
        case RunState::Kind::Stopping:
            if (state.identity != command.runtime_identity) {
                return reject(state, CommandRejection::StaleIdentity);
            }
            if (state.coroutine_settled) return reject(state, CommandRejection::DuplicateResult);
            if (state.persistence_settled) return release_settled_stop(state);
            {
                RunState next = state;
                next.coroutine_settled = true;
                return accepted(std::move(next), {});
            }
    }
    return reject(state, CommandRejection::IllegalState);
}

Transition settle_persistence(const RunState& state, const Command& command) {
    if (state.kind != RunState::Kind::Stopping) {
        return reject(state, CommandRejection::IllegalState);
    }
    if (!state.finalization_effect_id.has_value()) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    const RunEffectIdentity expected =
        effect_identity(state.identity, *state.finalization_effect_id);
    if (expected != command.identity) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    if (state.persistence_settled) return reject(state, CommandRejection::DuplicateResult);
    if (!command.success) {
        if (state.persistence_failure_reported) {
            return reject(state, CommandRejection::DuplicateResult);
        }
        RunState next = state;
        next.persistence_failure_reported = true;
        return accepted(std::move(next),
                        {effect_with_identity(Effect::Kind::StopPersistenceFailed,
                                              command.identity)});
    }
    if (state.coroutine_settled) return release_settled_stop(state);
    RunState next = state;
    next.persistence_settled = true;
    return accepted(std::move(next), {});
}

}  // namespace

bool operator==(const RuntimeRunIdentity& first, const RuntimeRunIdentity& second) {
    return first.conversation_id == second.conversation_id &&
           first.owner_token == second.owner_token && first.run_id == second.run_id &&
           first.pass == second.pass;
}

bool operator==(const RunEffectIdentity& first, const RunEffectIdentity& second) {
    return first.conversation_id == second.conversation_id &&
           first.owner_token == second.owner_token && first.run_id == second.run_id &&
           first.pass == second.pass && first.effect_id == second.effect_id;
}

bool operator==(const RunRecoverySnapshot& first, const RunRecoverySnapshot& second) {
    return first.conversation_id == second.conversation_id && first.run_id == second.run_id &&
           first.pass == second.pass && first.status == second.status;
}

Transition reduce(const RunState& state, const Command& command) {
    if (state.conversation_id != command_conversation_id(command)) {
        return reject(state, CommandRejection::StaleIdentity);
    }
    switch (command.kind) {
        case Command::Kind::Recover:
            return request_recovery(state, command);
        case Command::Kind::RecoveryCompleted:
            return complete_recovery(state, command);
        case Command::Kind::AcquireSlot:
            return acquire(state, command);
        case Command::Kind::SendRequested:
            return request_send(state, command);
        case Command::Kind::InputPersisted:
            return input_persisted(state, command);
        case Command::Kind::InputPersistenceFailed:
            return input_persistence_failed(state, command);
        case Command::Kind::SendLaunchAbandoned:
            return abandon_send_launch(state, command);
        case Command::Kind::BindRun:
            return bind_run(state, command);
        case Command::Kind::ProviderPassRequested:
            return request_provider_pass(state, command);
        case Command::Kind::ProviderPassCompleted:
            return complete_provider_pass(state, command);
        case Command::Kind::ToolBatchRequested:
            return request_tool_batch(state, command);
        case Command::Kind::ToolBatchCompleted:
            return complete_tool_batch(state, command);
        case Command::Kind::ToolRoundCommitted:
            return commit_tool_round(state, command);
        case Command::Kind::FinalizationRequested:
            return request_finalization(state, command);
        case Command::Kind::FinalizationCompleted:
            return complete_finalization(state, command);
        case Command::Kind::StopRequested:
            return request_stop(state, command);
        case Command::Kind::CoroutineSettled:
            return settle_coroutine(state, command);
        case Command::Kind::PersistenceSettled:
            return settle_persistence(state, command);
    }
    return reject(state, CommandRejection::IllegalState);
}

// ---- trace descriptions ---------------------------------------------------------------------

namespace {

const char* bool_text(bool value) { return value ? "true" : "false"; }

std::string null_or(const std::optional<std::string>& value) {
    return value.has_value() ? *value : "null";
}

std::string phase_text(const ProviderPhase& phase) {
    switch (phase.kind) {
        case ProviderPhase::Kind::None:
            return "None";
        case ProviderPhase::Kind::Running:
            return "Running(identity=" + describe(phase.identity) + ")";
    }
    return "None";
}

std::string tool_phase_text(const ToolPhase& phase) {
    switch (phase.kind) {
        case ToolPhase::Kind::None:
            return "None";
        case ToolPhase::Kind::Executing:
            return "Executing(batchIdentity=" + describe(phase.batch_identity) + ")";
        case ToolPhase::Kind::Committing:
            return "Committing(batchIdentity=" + describe(phase.batch_identity) +
                   ", commitIdentity=" + describe(phase.commit_identity) +
                   ", failureReported=" + bool_text(phase.failure_reported) + ")";
    }
    return "None";
}

}  // namespace

std::string describe(RunStatus status) {
    switch (status) {
        case RunStatus::Active: return "ACTIVE";
        case RunStatus::Stopping: return "STOPPING";
        case RunStatus::Completed: return "COMPLETED";
        case RunStatus::Stopped: return "STOPPED";
        case RunStatus::Failed: return "FAILED";
    }
    return "ACTIVE";
}

std::string describe(RunEndReason reason) {
    switch (reason) {
        case RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case RunEndReason::UserStopped: return "USER_STOPPED";
        case RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    return "MODEL_COMPLETED";
}

std::string describe(ProviderPassResult result) {
    switch (result) {
        case ProviderPassResult::CompletedText: return "COMPLETED_TEXT";
        case ProviderPassResult::CompletedToolCalls: return "COMPLETED_TOOL_CALLS";
        case ProviderPassResult::Truncated: return "TRUNCATED";
        case ProviderPassResult::Failed: return "FAILED";
        case ProviderPassResult::Cancelled: return "CANCELLED";
    }
    return "COMPLETED_TEXT";
}

std::string describe(SlotReleaseReason reason) {
    switch (reason) {
        case SlotReleaseReason::NormalCompletion: return "NORMAL_COMPLETION";
        case SlotReleaseReason::NormalFinalizationSettled: return "NORMAL_FINALIZATION_SETTLED";
        case SlotReleaseReason::StopBarriersSettled: return "STOP_BARRIERS_SETTLED";
        case SlotReleaseReason::EmptyStop: return "EMPTY_STOP";
        case SlotReleaseReason::SendLaunchAbandoned: return "SEND_LAUNCH_ABANDONED";
    }
    return "NORMAL_COMPLETION";
}

std::string describe(CommandRejection rejection) {
    switch (rejection) {
        case CommandRejection::IllegalState: return "ILLEGAL_STATE";
        case CommandRejection::StaleIdentity: return "STALE_IDENTITY";
        case CommandRejection::DuplicateResult: return "DUPLICATE_RESULT";
    }
    return "ILLEGAL_STATE";
}

std::string describe(const RuntimeRunIdentity& identity) {
    return "RuntimeRunIdentity(conversationId=" + identity.conversation_id +
           ", ownerToken=" + std::to_string(identity.owner_token) +
           ", runId=" + null_or(identity.run_id) + ", pass=" + std::to_string(identity.pass) + ")";
}

std::string describe(const RunEffectIdentity& identity) {
    return "RunEffectIdentity(conversationId=" + identity.conversation_id +
           ", ownerToken=" + std::to_string(identity.owner_token) + ", runId=" + identity.run_id +
           ", pass=" + std::to_string(identity.pass) + ", effectId=" + identity.effect_id + ")";
}

std::string describe(const RunRecoverySnapshot& snapshot) {
    return "RunRecoverySnapshot(conversationId=" + snapshot.conversation_id +
           ", runId=" + snapshot.run_id + ", pass=" + std::to_string(snapshot.pass) +
           ", status=" + describe(snapshot.status) + ")";
}

std::string describe(const RunState& state) {
    switch (state.kind) {
        case RunState::Kind::Idle:
            return "Idle(conversationId=" + state.conversation_id + ")";
        case RunState::Kind::Recovering:
            return "Recovering(snapshot=" + describe(state.snapshot) +
                   ", effectIdentity=" + describe(state.effect_identity) +
                   ", failureReported=" + bool_text(state.failure_reported) + ")";
        case RunState::Kind::Preparing:
            return "Preparing(ownerIdentity=" + describe(state.owner_identity) +
                   ", inputEffectIdentity=" + describe(state.input_effect_identity) +
                   ", inputFailureReported=" + bool_text(state.input_failure_reported) + ")";
        case RunState::Kind::Active:
            return "Active(identity=" + describe(state.identity) +
                   ", coroutineSettled=" + bool_text(state.coroutine_settled) +
                   ", providerPhase=" + phase_text(state.provider_phase) +
                   ", toolPhase=" + tool_phase_text(state.tool_phase) + ")";
        case RunState::Kind::Stopping:
            return "Stopping(identity=" + describe(state.identity) +
                   ", finalizationEffectId=" + null_or(state.finalization_effect_id) +
                   ", coroutineSettled=" + bool_text(state.coroutine_settled) +
                   ", persistenceSettled=" + bool_text(state.persistence_settled) +
                   ", persistenceFailureReported=" +
                   bool_text(state.persistence_failure_reported) + ")";
        case RunState::Kind::Finalizing:
            return "Finalizing(identity=" + describe(state.identity) +
                   ", effectIdentity=" + describe(state.effect_identity) +
                   ", status=" + describe(state.status) + ", reason=" + describe(state.reason) +
                   ", markConversationUnread=" + bool_text(state.mark_conversation_unread) +
                   ", coroutineSettled=" + bool_text(state.coroutine_settled) +
                   ", persistenceSettled=" + bool_text(state.persistence_settled) +
                   ", persistenceFailureReported=" +
                   bool_text(state.persistence_failure_reported) + ")";
    }
    return "Idle(conversationId=" + state.conversation_id + ")";
}

std::string describe(const Command& command) {
    switch (command.kind) {
        case Command::Kind::Recover:
            return "Recover(snapshot=" + describe(command.snapshot) + ")";
        case Command::Kind::RecoveryCompleted:
            return "RecoveryCompleted(identity=" + describe(command.identity) +
                   ", success=" + bool_text(command.success) + ")";
        case Command::Kind::AcquireSlot:
            return "AcquireSlot(identity=" + describe(command.runtime_identity) + ")";
        case Command::Kind::SendRequested:
            return "SendRequested(identity=" + describe(command.identity) +
                   ", directOnly=" + bool_text(command.direct_only) +
                   ", hasPendingGuidance=" + bool_text(command.has_pending_guidance) + ")";
        case Command::Kind::InputPersisted:
            return "InputPersisted(identity=" + describe(command.identity) + ")";
        case Command::Kind::InputPersistenceFailed:
            return "InputPersistenceFailed(identity=" + describe(command.identity) + ")";
        case Command::Kind::SendLaunchAbandoned:
            return "SendLaunchAbandoned(identity=" + describe(command.identity) + ")";
        case Command::Kind::BindRun:
            return "BindRun(identity=" + describe(command.runtime_identity) + ")";
        case Command::Kind::ProviderPassRequested:
            return "ProviderPassRequested(identity=" + describe(command.identity) + ")";
        case Command::Kind::ProviderPassCompleted:
            return "ProviderPassCompleted(identity=" + describe(command.identity) +
                   ", result=" + describe(command.result) + ")";
        case Command::Kind::ToolBatchRequested:
            return "ToolBatchRequested(identity=" + describe(command.identity) + ")";
        case Command::Kind::ToolBatchCompleted:
            return "ToolBatchCompleted(identity=" + describe(command.identity) + ")";
        case Command::Kind::ToolRoundCommitted:
            return "ToolRoundCommitted(identity=" + describe(command.identity) +
                   ", success=" + bool_text(command.success) + ")";
        case Command::Kind::FinalizationRequested:
            return "FinalizationRequested(identity=" + describe(command.identity) +
                   ", status=" + describe(command.status) +
                   ", reason=" + describe(command.reason) +
                   ", markConversationUnread=" + bool_text(command.mark_conversation_unread) + ")";
        case Command::Kind::FinalizationCompleted:
            return "FinalizationCompleted(identity=" + describe(command.identity) +
                   ", success=" + bool_text(command.success) + ")";
        case Command::Kind::StopRequested:
            return "StopRequested(identity=" + describe(command.runtime_identity) +
                   ", coroutineAlreadySettled=" + bool_text(command.coroutine_already_settled) +
                   ", requiresPersistence=" + bool_text(command.requires_persistence) +
                   ", effectId=" + null_or(command.effect_id) + ")";
        case Command::Kind::CoroutineSettled:
            return "CoroutineSettled(identity=" + describe(command.runtime_identity) + ")";
        case Command::Kind::PersistenceSettled:
            return "PersistenceSettled(identity=" + describe(command.identity) +
                   ", success=" + bool_text(command.success) + ")";
    }
    return "AcquireSlot(identity=" + describe(command.runtime_identity) + ")";
}

std::string describe(const Effect& effect) {
    const std::string identity = describe(effect.identity);
    switch (effect.kind) {
        case Effect::Kind::RecoverDurableRun:
            return "RecoverDurableRun(identity=" + identity +
                   ", priorStatus=" + describe(effect.prior_status) + ")";
        case Effect::Kind::RunRecoveryFailed:
            return "RunRecoveryFailed(identity=" + identity + ")";
        case Effect::Kind::SlotActivated:
            return "SlotActivated(identity=" + describe(effect.runtime_identity) + ")";
        case Effect::Kind::PersistAcceptedInput:
            return "PersistAcceptedInput(identity=" + identity + ")";
        case Effect::Kind::AcceptGuidance:
            return "AcceptGuidance(identity=" + identity + ")";
        case Effect::Kind::DrainGuidanceFirst:
            return "DrainGuidanceFirst(identity=" + identity + ")";
        case Effect::Kind::AwaitRunRelease:
            return "AwaitRunRelease(identity=" + identity + ")";
        case Effect::Kind::RejectSendBusy:
            return "RejectSendBusy(identity=" + identity + ")";
        case Effect::Kind::StartProviderPass:
            return "StartProviderPass(identity=" + identity + ")";
        case Effect::Kind::ProviderPassAccepted:
            return "ProviderPassAccepted(identity=" + identity +
                   ", result=" + describe(effect.result) + ")";
        case Effect::Kind::CancelProviderPass:
            return "CancelProviderPass(identity=" + describe(effect.runtime_identity) + ")";
        case Effect::Kind::FinalizeStop:
            return "FinalizeStop(identity=" + identity + ")";
        case Effect::Kind::StopPersistenceFailed:
            return "StopPersistenceFailed(identity=" + identity + ")";
        case Effect::Kind::ExecuteToolBatch:
            return "ExecuteToolBatch(identity=" + identity + ")";
        case Effect::Kind::CommitToolRound:
            return "CommitToolRound(identity=" + identity + ")";
        case Effect::Kind::ContinueProviderPass:
            return "ContinueProviderPass(identity=" + identity + ")";
        case Effect::Kind::ToolRoundCommitFailed:
            return "ToolRoundCommitFailed(identity=" + identity + ")";
        case Effect::Kind::FinalizeRun:
            return "FinalizeRun(identity=" + identity + ", status=" + describe(effect.status) +
                   ", reason=" + describe(effect.reason) + ", markConversationUnread=" +
                   bool_text(effect.mark_conversation_unread) + ")";
        case Effect::Kind::RunFinalizationFailed:
            return "RunFinalizationFailed(identity=" + identity + ")";
        case Effect::Kind::ReleaseSlot:
            return "ReleaseSlot(identity=" + describe(effect.runtime_identity) +
                   ", reason=" + describe(effect.release_reason) + ")";
    }
    return "RunRecoveryFailed(identity=" + identity + ")";
}

std::string describe(const std::vector<Effect>& effects) {
    std::string out = "[";
    for (std::size_t i = 0; i < effects.size(); ++i) {
        if (i > 0) out += ", ";
        out += describe(effects[i]);
    }
    out += "]";
    return out;
}

}  // namespace agora::core::conversation_runtime
