#pragma once

#include <cstdint>
#include <optional>
#include <string>
#include <vector>

namespace agora::core::native_run_archive {

/** data/model/RuntimeIdentity.kt: the durable lifecycle of one agentic execution. */
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

inline bool is_terminal(RunStatus status) {
    return status == RunStatus::Completed || status == RunStatus::Stopped ||
           status == RunStatus::Failed;
}

/** One imported v3 Run row, before the archive policy terminalizes it. */
struct ArchivedRunSnapshot {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    /** Raw stored enum name; valueOf is exact and case-sensitive. */
    std::string status;
    std::int64_t started_at = 0;
    std::int64_t last_checkpoint_at = 0;
    std::optional<std::int64_t> stop_requested_at;
    std::optional<std::int64_t> ended_at;
    /** Raw stored enum name; a null, invalid or blank reason falls back by status. */
    std::optional<std::string> end_reason;
    int current_pass = 0;
    bool legacy_ambiguous = false;
};

/** data/local/RunEntity.kt: the durable Run row written back after import. */
struct RunEntity {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    RunStatus status = RunStatus::Completed;
    std::optional<int> active_slot;
    std::int64_t started_at = 0;
    std::int64_t last_checkpoint_at = 0;
    std::optional<std::int64_t> stop_requested_at;
    std::optional<std::int64_t> ended_at;
    std::optional<RunEndReason> end_reason;
    int current_pass = 0;
    bool legacy_ambiguous = false;
};

/** One archived message -> Run ownership row. */
struct ArchivedMessageRunOwnership {
    std::string message_id;
    std::string conversation_id;
    std::optional<std::string> run_id;
    std::optional<std::int64_t> run_sequence;
    std::optional<int> consumed_at_pass;
};

/**
 * Port of data/NativeRunArchivePolicy.kt.
 *
 * The archive is accepted as one complete graph or rejected whole. `terminalize` always writes a
 * terminal Run with an endedAt and an end reason, `has_complete_ownership` validates every
 * message -> Run link, and `order_by_parent` produces a parent-before-child insertion order while
 * preserving missing parents and cycles as ambiguous roots.
 */

/** Repairs one imported Run: nonterminal statuses become STOPPED + PROCESS_RECOVERED. */
RunEntity terminalize(const ArchivedRunSnapshot& snapshot);

/**
 * True only when every message has a unique id, every ownership row names an existing Run in the
 * same conversation with a non-negative sequence, and no (runId, sequence) pair repeats.
 */
bool has_complete_ownership(const std::vector<RunEntity>& runs,
                            const std::vector<ArchivedMessageRunOwnership>& ownership,
                            bool source_run_ids_were_unique);

/**
 * Parent-before-child order, sorted by startedAt then id at each ready step.
 *
 * A missing parent becomes a root marked ambiguous. A cycle is broken by marking the smallest
 * remaining (startedAt, id) Run as a root and ambiguous. Duplicate run ids behave like
 * `associateByTo(linkedMapOf())`: the last value keeps the first insertion position.
 */
std::vector<RunEntity> order_by_parent(const std::vector<RunEntity>& raw_runs);

}  // namespace agora::core::native_run_archive
