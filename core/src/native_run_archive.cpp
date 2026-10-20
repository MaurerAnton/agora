// Port of data/NativeRunArchivePolicy.kt.
#include "agora/core/native_run_archive.hpp"

#include <algorithm>
#include <cstdint>
#include <optional>
#include <set>
#include <stdexcept>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "utf16.hpp"

namespace agora::core::native_run_archive {
namespace {

std::optional<RunStatus> parse_status(std::string_view value) {
    if (value == "ACTIVE") return RunStatus::Active;
    if (value == "STOPPING") return RunStatus::Stopping;
    if (value == "COMPLETED") return RunStatus::Completed;
    if (value == "STOPPED") return RunStatus::Stopped;
    if (value == "FAILED") return RunStatus::Failed;
    return std::nullopt;
}

std::optional<RunEndReason> parse_end_reason(std::string_view value) {
    if (value == "MODEL_COMPLETED") return RunEndReason::ModelCompleted;
    if (value == "USER_STOPPED") return RunEndReason::UserStopped;
    if (value == "PROCESS_RECOVERED") return RunEndReason::ProcessRecovered;
    if (value == "PROVIDER_ERROR") return RunEndReason::ProviderError;
    return std::nullopt;
}

/** `compareBy<RunEntity> { it.startedAt }.thenBy { it.id }` over Kotlin String order. */
bool run_less(const RunEntity& lhs, const RunEntity& rhs) {
    if (lhs.started_at != rhs.started_at) return lhs.started_at < rhs.started_at;
    return utf16::from_utf8(lhs.id) < utf16::from_utf8(rhs.id);
}

std::optional<std::int64_t> optional_max(std::optional<std::int64_t> value,
                                        std::int64_t floor) {
    return std::max(value.value_or(floor), floor);
}

}  // namespace

RunEntity terminalize(const ArchivedRunSnapshot& snapshot) {
    // runCatching { RunStatus.valueOf(status) }.getOrDefault(FAILED)
    const RunStatus imported_status = parse_status(snapshot.status).value_or(RunStatus::Failed);
    const bool terminal = is_terminal(imported_status);
    const RunStatus terminal_status = terminal ? imported_status : RunStatus::Stopped;

    std::optional<RunEndReason> terminal_reason;
    if (!terminal) {
        terminal_reason = RunEndReason::ProcessRecovered;
    } else {
        // runCatching { endReason?.let(RunEndReason::valueOf) }.getOrNull() ?: when (status)
        if (snapshot.end_reason.has_value()) {
            terminal_reason = parse_end_reason(*snapshot.end_reason);
        }
        if (!terminal_reason.has_value()) {
            switch (terminal_status) {
                case RunStatus::Completed:
                    terminal_reason = RunEndReason::ModelCompleted;
                    break;
                case RunStatus::Stopped:
                    terminal_reason = RunEndReason::UserStopped;
                    break;
                case RunStatus::Failed:
                    terminal_reason = RunEndReason::ProviderError;
                    break;
                default:
                    throw std::logic_error("Terminal status expected");
            }
        }
    }

    const std::int64_t checkpoint =
        std::max<std::int64_t>(snapshot.started_at, snapshot.last_checkpoint_at);

    RunEntity entity;
    entity.id = snapshot.id;
    entity.conversation_id = snapshot.conversation_id;
    entity.parent_run_id = snapshot.parent_run_id;
    entity.status = terminal_status;
    entity.active_slot = std::nullopt;
    entity.started_at = snapshot.started_at;
    entity.last_checkpoint_at = checkpoint;
    entity.stop_requested_at = snapshot.stop_requested_at;
    entity.ended_at = optional_max(snapshot.ended_at, checkpoint);
    entity.end_reason = terminal_reason;
    entity.current_pass = std::max(snapshot.current_pass, 0);
    entity.legacy_ambiguous = snapshot.legacy_ambiguous;
    return entity;
}

bool has_complete_ownership(const std::vector<RunEntity>& runs,
                            const std::vector<ArchivedMessageRunOwnership>& ownership,
                            bool source_run_ids_were_unique) {
    if (runs.empty() || !source_run_ids_were_unique) return false;

    std::unordered_set<std::string> message_ids;
    for (const ArchivedMessageRunOwnership& item : ownership) {
        message_ids.insert(item.message_id);
    }
    if (message_ids.size() != ownership.size()) return false;

    // associateBy(RunEntity::id): a duplicated id keeps the last entity.
    std::unordered_map<std::string, const RunEntity*> runs_by_id;
    runs_by_id.reserve(runs.size());
    for (const RunEntity& run : runs) runs_by_id[run.id] = &run;

    std::set<std::pair<std::string, std::int64_t>> sequence_keys;
    for (const ArchivedMessageRunOwnership& item : ownership) {
        if (!item.run_id.has_value() || !item.run_sequence.has_value()) return false;
        const auto run = runs_by_id.find(*item.run_id);
        if (run == runs_by_id.end()) return false;
        if (run->second->conversation_id != item.conversation_id) return false;
        if (*item.run_sequence < 0) return false;
        if (!sequence_keys.emplace(run->second->id, *item.run_sequence).second) return false;
    }
    return true;
}

std::vector<RunEntity> order_by_parent(const std::vector<RunEntity>& raw_runs) {
    if (raw_runs.empty()) return {};

    std::unordered_set<std::string> ids;
    ids.reserve(raw_runs.size());
    for (const RunEntity& run : raw_runs) ids.insert(run.id);

    // `associateByTo(linkedMapOf())`: a repeated id keeps its first position and its last value.
    std::vector<std::pair<std::string, RunEntity>> remaining;
    remaining.reserve(raw_runs.size());
    for (const RunEntity& raw : raw_runs) {
        RunEntity run = raw;
        if (run.parent_run_id.has_value() && ids.find(*run.parent_run_id) == ids.end()) {
            run.parent_run_id = std::nullopt;
            run.legacy_ambiguous = true;
        }
        const auto existing = std::find_if(
            remaining.begin(), remaining.end(),
            [&](const std::pair<std::string, RunEntity>& entry) { return entry.first == run.id; });
        if (existing == remaining.end()) {
            remaining.emplace_back(run.id, std::move(run));
        } else {
            existing->second = std::move(run);
        }
    }

    std::vector<RunEntity> ordered;
    ordered.reserve(remaining.size());
    std::unordered_set<std::string> inserted;

    while (!remaining.empty()) {
        std::vector<const RunEntity*> ready;
        ready.reserve(remaining.size());
        for (const auto& entry : remaining) {
            const RunEntity& run = entry.second;
            if (!run.parent_run_id.has_value() ||
                inserted.find(*run.parent_run_id) != inserted.end()) {
                ready.push_back(&run);
            }
        }
        std::stable_sort(ready.begin(), ready.end(),
                         [](const RunEntity* lhs, const RunEntity* rhs) {
                             return run_less(*lhs, *rhs);
                         });

        if (ready.empty()) {
            // No remaining Run has an inserted parent: break one cycle edge at the smallest Run.
            const auto first = std::min_element(
                remaining.begin(), remaining.end(),
                [](const std::pair<std::string, RunEntity>& lhs,
                   const std::pair<std::string, RunEntity>& rhs) {
                    return run_less(lhs.second, rhs.second);
                });
            first->second.parent_run_id = std::nullopt;
            first->second.legacy_ambiguous = true;
            continue;
        }

        std::vector<std::string> ready_ids;
        ready_ids.reserve(ready.size());
        for (const RunEntity* run : ready) {
            ordered.push_back(*run);
            inserted.insert(run->id);
            ready_ids.push_back(run->id);
        }
        for (const std::string& id : ready_ids) {
            remaining.erase(
                std::find_if(remaining.begin(), remaining.end(),
                             [&](const std::pair<std::string, RunEntity>& entry) {
                                 return entry.first == id;
                             }));
        }
    }
    return ordered;
}

}  // namespace agora::core::native_run_archive
