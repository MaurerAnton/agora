#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::legacy_run_backfill {

// Port of data/local/migration/LegacyRunBackfillPlanner.kt: the v16 message forest -> v17 Run
// backfill used by Migration16To17. Every visible USER starts a Run; model and synthetic
// tool/result descendants stay in that Run until the next visible USER. Historical model siblings
// are grouped losslessly and marked legacy-ambiguous instead of being split.

enum class Participant {
    User,
    Model,
    Error,
};

enum class MessageStatus {
    Transcribing,
    Sending,
    Thinking,
    ToolCalling,
    Success,
    Stopped,
    Error,
};

enum class RunStatus {
    Active,
    Stopping,
    Completed,
    Stopped,
    Failed,
};

enum class RunEndReason {
    ModelCompleted,
    UserStopped,
    ProcessRecovered,
    ProviderError,
};

struct LegacyMessageRecord {
    std::string id;
    std::optional<std::string> parent_id;
    Participant participant = Participant::User;
    MessageStatus status = MessageStatus::Success;
    long long timestamp = 0;
    bool is_synthetic = false;  // Kotlin default: id starts with tool_ or result_
};

struct PlannedLegacyRun {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    std::optional<std::string> boundary_message_id;
    RunStatus status = RunStatus::Completed;
    RunEndReason end_reason = RunEndReason::ModelCompleted;
    long long started_at = 0;
    long long ended_at = 0;
    bool legacy_ambiguous = false;
};

struct PlannedMessageAssignment {
    std::string message_id;
    std::string run_id;
    long long run_sequence = 0;
    std::optional<int> consumed_at_pass;
};

struct LegacyRunBackfillPlan {
    std::vector<PlannedLegacyRun> runs;
    std::vector<PlannedMessageAssignment> assignments;
};

/**
 * Kotlin `require` failures throw IllegalArgumentException; the port returns the same message in
 * `error` instead. Run ids are deterministic UUID v3 values (UUID.nameUUIDFromBytes).
 */
struct PlanResult {
    bool ok = true;
    std::string error;
    LegacyRunBackfillPlan plan;
};

PlanResult plan(std::string_view conversation_id, const std::vector<LegacyMessageRecord>& messages);

using NullableSelections = std::vector<std::pair<std::optional<std::string>, std::string>>;

/**
 * selectedRunBranches: derives Run-level selections from legacy message-level selections, walking
 * every Run boundary and choosing the selected (or last) visible child. Entries are emitted in Run
 * plan order with the nullable root key first.
 */
struct SelectionResult {
    bool ok = true;
    std::string error;
    NullableSelections selections;
};

SelectionResult selected_run_branches(
    const std::vector<LegacyMessageRecord>& messages, const LegacyRunBackfillPlan& plan,
    const NullableSelections& selected_message_branches);

/** Kotlin data-class toString() of the planned run. */
std::string describe(const PlannedLegacyRun& run);

/** Kotlin data-class toString() of the planned assignment. */
std::string describe(const PlannedMessageAssignment& assignment);

}  // namespace agora::core::legacy_run_backfill
