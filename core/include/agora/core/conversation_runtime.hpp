#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agora::core::conversation_runtime {

/**
 * Port of model/ConversationRuntimeReducer.kt and the types it owns.
 *
 * The Kotlin cluster (RunState.kt, ConversationCommand.kt, RunEffect.kt,
 * RuntimeTransition.kt, RuntimeIdentity.kt, ConversationRuntimeReducer.kt) has no imports at all,
 * so the golden stand compiles it verbatim and every branch here is pinned by a command sequence.
 *
 * The representation is a wide struct per type rather than a variant: the reducer is a state
 * machine whose fields are inspected by kind, and a flat record keeps the transliteration close
 * to the Kotlin `copy(...)` calls, which is what makes review possible.
 */

enum class RunStatus { Active, Stopping, Completed, Stopped, Failed };

inline bool is_terminal(RunStatus status) {
    return status == RunStatus::Completed || status == RunStatus::Stopped ||
           status == RunStatus::Failed;
}

enum class RunEndReason { ModelCompleted, UserStopped, ProcessRecovered, ProviderError };

enum class ProviderPassResult {
    CompletedText,
    CompletedToolCalls,
    Truncated,
    Failed,
    Cancelled,
};

enum class SlotReleaseReason {
    NormalCompletion,
    NormalFinalizationSettled,
    StopBarriersSettled,
    EmptyStop,
    SendLaunchAbandoned,
};

enum class CommandRejection { IllegalState, StaleIdentity, DuplicateResult };

/** Identity of the in-process owner for one conversation Run. */
struct RuntimeRunIdentity {
    std::string conversation_id;
    std::int64_t owner_token = 0;
    std::optional<std::string> run_id;
    int pass = 0;
};

bool operator==(const RuntimeRunIdentity& first, const RuntimeRunIdentity& second);
inline bool operator!=(const RuntimeRunIdentity& first, const RuntimeRunIdentity& second) {
    return !(first == second);
}

/** Identity of one asynchronous effect. `run_id` is always bound. */
struct RunEffectIdentity {
    std::string conversation_id;
    std::int64_t owner_token = 0;
    std::string run_id;
    int pass = 0;
    std::string effect_id;
};

bool operator==(const RunEffectIdentity& first, const RunEffectIdentity& second);
inline bool operator!=(const RunEffectIdentity& first, const RunEffectIdentity& second) {
    return !(first == second);
}

/** Durable process-start snapshot for one live Run. */
struct RunRecoverySnapshot {
    std::string conversation_id;
    std::string run_id;
    int pass = 0;
    RunStatus status = RunStatus::Active;
};

bool operator==(const RunRecoverySnapshot& first, const RunRecoverySnapshot& second);
inline bool operator!=(const RunRecoverySnapshot& first, const RunRecoverySnapshot& second) {
    return !(first == second);
}

struct ProviderPhase {
    enum class Kind { None, Running };
    Kind kind = Kind::None;
    RunEffectIdentity identity;
};

struct ToolPhase {
    enum class Kind { None, Executing, Committing };
    Kind kind = Kind::None;
    RunEffectIdentity batch_identity;
    RunEffectIdentity commit_identity;
    bool failure_reported = false;
};

/** The conversation runtime state. Only the fields of `kind` are meaningful. */
struct RunState {
    enum class Kind { Idle, Recovering, Preparing, Active, Stopping, Finalizing };

    Kind kind = Kind::Idle;
    std::string conversation_id;

    // Recovering
    RunRecoverySnapshot snapshot;
    RunEffectIdentity effect_identity;
    bool failure_reported = false;
    // Preparing
    RuntimeRunIdentity owner_identity;
    RunEffectIdentity input_effect_identity;
    bool input_failure_reported = false;
    // Active / Stopping / Finalizing
    RuntimeRunIdentity identity;
    bool coroutine_settled = false;
    ProviderPhase provider_phase;
    ToolPhase tool_phase;
    std::optional<std::string> finalization_effect_id;
    bool persistence_settled = false;
    bool persistence_failure_reported = false;
    // Finalizing
    RunStatus status = RunStatus::Completed;
    RunEndReason reason = RunEndReason::ModelCompleted;
    bool mark_conversation_unread = false;
};

struct Command {
    enum class Kind {
        Recover,
        RecoveryCompleted,
        AcquireSlot,
        SendRequested,
        InputPersisted,
        InputPersistenceFailed,
        SendLaunchAbandoned,
        BindRun,
        ProviderPassRequested,
        ProviderPassCompleted,
        ToolBatchRequested,
        ToolBatchCompleted,
        ToolRoundCommitted,
        FinalizationRequested,
        FinalizationCompleted,
        StopRequested,
        CoroutineSettled,
        PersistenceSettled,
    };

    Kind kind = Kind::AcquireSlot;
    RunRecoverySnapshot snapshot;        // Recover
    RunEffectIdentity identity;          // every effect-identity command
    RuntimeRunIdentity runtime_identity; // AcquireSlot, BindRun, StopRequested, CoroutineSettled
    bool direct_only = false;            // SendRequested
    bool has_pending_guidance = false;   // SendRequested
    bool success = false;                // *Completed, ToolRoundCommitted, PersistenceSettled
    ProviderPassResult result = ProviderPassResult::CompletedText;
    RunStatus status = RunStatus::Completed;
    RunEndReason reason = RunEndReason::ModelCompleted;
    bool mark_conversation_unread = false;
    bool coroutine_already_settled = false; // StopRequested
    bool requires_persistence = false;      // StopRequested
    std::optional<std::string> effect_id;   // StopRequested
};

struct Effect {
    enum class Kind {
        RecoverDurableRun,
        RunRecoveryFailed,
        SlotActivated,
        PersistAcceptedInput,
        AcceptGuidance,
        DrainGuidanceFirst,
        AwaitRunRelease,
        RejectSendBusy,
        StartProviderPass,
        ProviderPassAccepted,
        CancelProviderPass,
        FinalizeStop,
        StopPersistenceFailed,
        ExecuteToolBatch,
        CommitToolRound,
        ContinueProviderPass,
        ToolRoundCommitFailed,
        FinalizeRun,
        RunFinalizationFailed,
        ReleaseSlot,
    };

    Kind kind = Kind::RecoverDurableRun;
    RunEffectIdentity identity;
    RuntimeRunIdentity runtime_identity;
    RunStatus prior_status = RunStatus::Active; // RecoverDurableRun
    ProviderPassResult result = ProviderPassResult::CompletedText;
    RunStatus status = RunStatus::Completed;            // FinalizeRun
    RunEndReason reason = RunEndReason::ModelCompleted; // FinalizeRun
    bool mark_conversation_unread = false;              // FinalizeRun
    SlotReleaseReason release_reason = SlotReleaseReason::NormalCompletion;
};

struct Transition {
    RunState state;
    std::vector<Effect> effects;
    std::optional<CommandRejection> rejection;

    bool accepted() const { return !rejection.has_value(); }
};

Transition reduce(const RunState& state, const Command& command);

/** Kotlin data-class `toString()`, reproduced field for field for the golden traces. */
std::string describe(const RuntimeRunIdentity& identity);
std::string describe(const RunEffectIdentity& identity);
std::string describe(const RunRecoverySnapshot& snapshot);
std::string describe(const RunState& state);
std::string describe(const Command& command);
std::string describe(const Effect& effect);
std::string describe(const std::vector<Effect>& effects);

std::string describe(RunStatus status);
std::string describe(RunEndReason reason);
std::string describe(ProviderPassResult result);
std::string describe(SlotReleaseReason reason);
std::string describe(CommandRejection rejection);

}  // namespace agora::core::conversation_runtime
