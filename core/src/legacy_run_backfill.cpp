// Port of data/local/migration/LegacyRunBackfillPlanner.kt.
//
// Kotlin semantics kept by the port: associateBy/groupBy first-seen order, a min-heap
// PriorityQueue keyed by (timestamp, id), linked map/set insertion order, `getOrPut` creating a
// Run builder only on first encounter, and deterministic UUID v3 Run ids (MD5 name UUID).
#include "agora/core/legacy_run_backfill.hpp"

#include <algorithm>
#include <optional>
#include <queue>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>
#include <utility>
#include <vector>

#include "agora/core/json.hpp"
#include "agora/core/uuid_v3.hpp"

namespace agora::core::legacy_run_backfill {
namespace {

/** compareBy { timestamp }.thenBy { id }; app ids are ASCII. */
bool message_less(const LegacyMessageRecord& left, const LegacyMessageRecord& right) {
    if (left.timestamp != right.timestamp) return left.timestamp < right.timestamp;
    return left.id < right.id;
}

using NullableKey = std::optional<std::string>;
using NullableKeyMap = std::vector<std::pair<NullableKey, std::string>>;

void set_nullable(NullableKeyMap& map, const NullableKey& key, const std::string& value) {
    for (auto& entry : map) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    map.emplace_back(key, value);
}

const std::string* find_nullable(const NullableKeyMap& map, const NullableKey& key) {
    for (const auto& entry : map) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

std::string nullable_text(const std::optional<std::string>& value) {
    return value.has_value() ? *value : "null";
}

const char* status_name(RunStatus status) {
    switch (status) {
        case RunStatus::Active: return "ACTIVE";
        case RunStatus::Stopping: return "STOPPING";
        case RunStatus::Completed: return "COMPLETED";
        case RunStatus::Stopped: return "STOPPED";
        case RunStatus::Failed: return "FAILED";
    }
    return "COMPLETED";
}

const char* reason_name(RunEndReason reason) {
    switch (reason) {
        case RunEndReason::ModelCompleted: return "MODEL_COMPLETED";
        case RunEndReason::UserStopped: return "USER_STOPPED";
        case RunEndReason::ProcessRecovered: return "PROCESS_RECOVERED";
        case RunEndReason::ProviderError: return "PROVIDER_ERROR";
    }
    return "MODEL_COMPLETED";
}

bool is_stopped_status(MessageStatus status) {
    return status == MessageStatus::Stopped || status == MessageStatus::Transcribing ||
           status == MessageStatus::Sending || status == MessageStatus::Thinking ||
           status == MessageStatus::ToolCalling;
}

struct Builder {
    std::string id;
    std::string conversation_id;
    std::optional<std::string> parent_run_id;
    std::optional<std::string> boundary_message_id;
    bool legacy_ambiguous = false;
    std::vector<const LegacyMessageRecord*> messages;
};

/** `messages.groupBy { it.parentId }` including the nullable root group, first-seen order. */
struct NullableChildren {
    std::vector<std::pair<NullableKey, std::vector<const LegacyMessageRecord*>>> groups;
};

const std::vector<const LegacyMessageRecord*>& children_of(const NullableChildren& children,
                                                           const NullableKey& parent_id) {
    static const std::vector<const LegacyMessageRecord*> kEmpty;
    for (const auto& entry : children.groups) {
        if (entry.first == parent_id) return entry.second;
    }
    return kEmpty;
}

/** chooseChild: (timestamp, id) order, synthetic rows only when no visible child exists. */
const LegacyMessageRecord* choose_child(const std::vector<const LegacyMessageRecord*>& children,
                                        const std::optional<std::string>& selected_id) {
    if (children.empty()) return nullptr;
    std::vector<const LegacyMessageRecord*> ordered = children;
    std::stable_sort(ordered.begin(), ordered.end(),
                     [](const LegacyMessageRecord* left, const LegacyMessageRecord* right) {
                         return message_less(*left, *right);
                     });
    std::vector<const LegacyMessageRecord*> visible;
    for (const LegacyMessageRecord* child : ordered) {
        if (!child->is_synthetic) visible.push_back(child);
    }
    const std::vector<const LegacyMessageRecord*>& candidates =
        visible.empty() ? ordered : visible;
    if (selected_id.has_value()) {
        for (const LegacyMessageRecord* child : candidates) {
            if (child->id == *selected_id) return child;
        }
    }
    return candidates.back();
}

PlannedLegacyRun to_planned_run(const Builder& builder) {
    bool failed = false;
    bool stopped = false;
    long long started_at = builder.messages.front()->timestamp;
    long long ended_at = started_at;
    for (const LegacyMessageRecord* message : builder.messages) {
        started_at = std::min(started_at, message->timestamp);
        ended_at = std::max(ended_at, message->timestamp);
        if (message->participant == Participant::Error ||
            message->status == MessageStatus::Error) {
            failed = true;
        }
        if (is_stopped_status(message->status)) stopped = true;
    }

    PlannedLegacyRun run;
    run.id = builder.id;
    run.conversation_id = builder.conversation_id;
    run.parent_run_id = builder.parent_run_id;
    run.boundary_message_id = builder.boundary_message_id;
    if (failed) {
        run.status = RunStatus::Failed;
        run.end_reason = RunEndReason::ProviderError;
    } else if (stopped) {
        run.status = RunStatus::Stopped;
        run.end_reason = RunEndReason::ProcessRecovered;
    } else {
        run.status = RunStatus::Completed;
        run.end_reason = RunEndReason::ModelCompleted;
    }
    run.started_at = started_at;
    run.ended_at = ended_at;
    run.legacy_ambiguous = builder.legacy_ambiguous;
    return run;
}

}  // namespace

PlanResult plan(std::string_view conversation_id,
                const std::vector<LegacyMessageRecord>& messages) {
    PlanResult result;
    if (json::trim(conversation_id).empty()) {
        result.ok = false;
        result.error = "conversationId must not be blank";
        return result;
    }

    std::unordered_map<std::string, const LegacyMessageRecord*> records_by_id;
    records_by_id.reserve(messages.size());
    for (const LegacyMessageRecord& message : messages) records_by_id[message.id] = &message;
    if (records_by_id.size() != messages.size()) {
        result.ok = false;
        result.error = "Legacy message IDs must be unique";
        return result;
    }
    if (messages.empty()) return result;

    // childrenByParent in plan(): only parents that exist in the record set.
    std::unordered_map<std::string, std::vector<const LegacyMessageRecord*>> children_by_parent;
    std::vector<std::string> parent_order;
    for (const LegacyMessageRecord& message : messages) {
        if (!message.parent_id.has_value()) continue;
        if (records_by_id.find(*message.parent_id) == records_by_id.end()) continue;
        auto [it, inserted] = children_by_parent.try_emplace(*message.parent_id);
        if (inserted) parent_order.push_back(*message.parent_id);
        it->second.push_back(&message);
    }

    // stableTopologyOrder: Kahn's algorithm over input-order buckets with a (timestamp, id) heap.
    std::unordered_map<std::string, long long> remaining_parents;
    remaining_parents.reserve(messages.size());
    const auto greater = [](const LegacyMessageRecord* left, const LegacyMessageRecord* right) {
        return message_less(*right, *left);
    };
    std::priority_queue<const LegacyMessageRecord*, std::vector<const LegacyMessageRecord*>,
                        decltype(greater)>
        ready(greater);
    for (const LegacyMessageRecord& message : messages) {
        const long long remaining =
            (message.parent_id.has_value() &&
             records_by_id.find(*message.parent_id) != records_by_id.end())
                ? 1
                : 0;
        remaining_parents[message.id] = remaining;
        if (remaining == 0) ready.push(&message);
    }

    std::vector<const LegacyMessageRecord*> ordered;
    ordered.reserve(messages.size());
    std::unordered_set<std::string> visited;
    visited.reserve(messages.size());
    while (!ready.empty()) {
        const LegacyMessageRecord* message = ready.top();
        ready.pop();
        if (!visited.insert(message->id).second) continue;
        ordered.push_back(message);
        const auto children_it = children_by_parent.find(message->id);
        if (children_it == children_by_parent.end()) continue;
        for (const LegacyMessageRecord* child : children_it->second) {
            auto remaining_it = remaining_parents.find(child->id);
            if (remaining_it == remaining_parents.end()) continue;
            const long long remaining = remaining_it->second - 1;
            remaining_it->second = remaining;
            if (remaining == 0) ready.push(child);
        }
    }

    // Corrupt parent cycles have no topological root: keep them deterministically.
    std::vector<const LegacyMessageRecord*> leftovers;
    for (const LegacyMessageRecord& message : messages) {
        if (visited.find(message.id) == visited.end()) leftovers.push_back(&message);
    }
    std::stable_sort(leftovers.begin(), leftovers.end(),
                     [](const LegacyMessageRecord* left, const LegacyMessageRecord* right) {
                         return message_less(*left, *right);
                     });
    ordered.insert(ordered.end(), leftovers.begin(), leftovers.end());

    std::vector<Builder> builders;
    std::unordered_map<std::string, std::size_t> builder_by_id;
    std::unordered_map<std::string, PlannedMessageAssignment> assignments_by_message;
    std::unordered_map<std::string, long long> next_sequence_by_run;

    for (const LegacyMessageRecord* message : ordered) {
        const PlannedMessageAssignment* parent_assignment = nullptr;
        if (message->parent_id.has_value()) {
            const auto assignment_it = assignments_by_message.find(*message->parent_id);
            if (assignment_it != assignments_by_message.end()) {
                parent_assignment = &assignment_it->second;
            }
        }

        const bool starts_run =
            message->participant == Participant::User && !message->is_synthetic;

        std::string run_id;
        if (starts_run) {
            run_id = uuid_v3("legacy-run:" + std::string(conversation_id) + ":user:" +
                             message->id);
        } else if (parent_assignment != nullptr) {
            run_id = parent_assignment->run_id;
        } else {
            run_id = uuid_v3("legacy-run:" + std::string(conversation_id) + ":orphan:" +
                             message->id);
        }

        std::size_t builder_index = 0;
        const auto builder_it = builder_by_id.find(run_id);
        if (builder_it == builder_by_id.end()) {
            Builder builder;
            builder.id = run_id;
            builder.conversation_id = std::string(conversation_id);
            if (starts_run) {
                if (parent_assignment != nullptr) {
                    builder.parent_run_id = parent_assignment->run_id;
                }
                builder.boundary_message_id = message->id;
            }
            builder.legacy_ambiguous =
                !starts_run ||
                (message->parent_id.has_value() &&
                 records_by_id.find(*message->parent_id) == records_by_id.end());
            builder_index = builders.size();
            builders.push_back(std::move(builder));
            builder_by_id.emplace(run_id, builder_index);
        } else {
            builder_index = builder_it->second;
        }
        builders[builder_index].messages.push_back(message);

        const auto sequence_it = next_sequence_by_run.find(run_id);
        const long long sequence =
            sequence_it == next_sequence_by_run.end() ? 0 : sequence_it->second;
        PlannedMessageAssignment assignment;
        assignment.message_id = message->id;
        assignment.run_id = run_id;
        assignment.run_sequence = sequence;
        if (starts_run) assignment.consumed_at_pass = 0;
        assignments_by_message[message->id] = assignment;
        next_sequence_by_run[run_id] = sequence + 1;
    }

    for (const std::string& parent_id : parent_order) {
        std::unordered_map<std::string, int> counts;
        for (const LegacyMessageRecord* child : children_by_parent[parent_id]) {
            if (child->is_synthetic) continue;
            const auto assignment_it = assignments_by_message.find(child->id);
            if (assignment_it == assignments_by_message.end()) continue;
            ++counts[assignment_it->second.run_id];
        }
        for (const auto& [run_id, count] : counts) {
            if (count <= 1) continue;
            const auto builder_it = builder_by_id.find(run_id);
            if (builder_it != builder_by_id.end()) {
                builders[builder_it->second].legacy_ambiguous = true;
            }
        }
    }

    result.plan.runs.reserve(builders.size());
    for (const Builder& builder : builders) {
        result.plan.runs.push_back(to_planned_run(builder));
    }
    result.plan.assignments.reserve(ordered.size());
    for (const LegacyMessageRecord* message : ordered) {
        const auto assignment_it = assignments_by_message.find(message->id);
        if (assignment_it != assignments_by_message.end()) {
            result.plan.assignments.push_back(assignment_it->second);
        }
    }
    return result;
}

SelectionResult selected_run_branches(
    const std::vector<LegacyMessageRecord>& messages, const LegacyRunBackfillPlan& plan,
    const NullableSelections& selected_message_branches) {
    SelectionResult result;
    if (messages.empty() || plan.runs.empty()) return result;

    std::unordered_map<std::string, const PlannedMessageAssignment*> assignment_by_message;
    assignment_by_message.reserve(plan.assignments.size());
    for (const PlannedMessageAssignment& assignment : plan.assignments) {
        assignment_by_message[assignment.message_id] = &assignment;
    }
    for (const LegacyMessageRecord& message : messages) {
        if (assignment_by_message.find(message.id) == assignment_by_message.end()) {
            result.ok = false;
            result.error = "Every legacy message must have a planned Run assignment";
            return result;
        }
    }

    NullableChildren children;
    {
        std::unordered_map<std::string, std::size_t> null_group;
        for (const LegacyMessageRecord& message : messages) {
            if (!message.parent_id.has_value()) {
                if (null_group.empty()) {
                    null_group[""] = 0;
                    children.groups.emplace_back(std::nullopt, std::vector<const LegacyMessageRecord*>{});
                }
                children.groups[0].second.push_back(&message);
                continue;
            }
            std::size_t index = 0;
            bool found = false;
            for (std::size_t i = 0; i < children.groups.size(); ++i) {
                if (children.groups[i].first.has_value() &&
                    *children.groups[i].first == *message.parent_id) {
                    index = i;
                    found = true;
                    break;
                }
            }
            if (!found) {
                index = children.groups.size();
                children.groups.emplace_back(message.parent_id,
                                             std::vector<const LegacyMessageRecord*>{});
            }
            children.groups[index].second.push_back(&message);
        }
    }

    auto lookup_selection = [&](const std::optional<std::string>& key) -> std::optional<std::string> {
        const std::string* found = find_nullable(selected_message_branches, key);
        if (found == nullptr) return std::nullopt;
        return *found;
    };

    const LegacyMessageRecord* root =
        choose_child(children_of(children, std::nullopt), lookup_selection(std::nullopt));
    if (root != nullptr) {
        const auto assignment_it = assignment_by_message.find(root->id);
        if (assignment_it != assignment_by_message.end()) {
            set_nullable(result.selections, std::nullopt, assignment_it->second->run_id);
        }
    }

    for (const PlannedLegacyRun& run : plan.runs) {
        if (!run.boundary_message_id.has_value()) continue;
        std::string cursor = *run.boundary_message_id;
        std::unordered_set<std::string> visited;
        while (visited.insert(cursor).second) {
            const LegacyMessageRecord* child =
                choose_child(children_of(children, cursor), lookup_selection(cursor));
            if (child == nullptr) break;
            const auto assignment_it = assignment_by_message.find(child->id);
            if (assignment_it == assignment_by_message.end()) break;
            const std::string& child_run_id = assignment_it->second->run_id;
            if (child_run_id != run.id) {
                set_nullable(result.selections, run.id, child_run_id);
                break;
            }
            cursor = child->id;
        }
    }
    return result;
}

std::string describe(const PlannedLegacyRun& run) {
    return "PlannedLegacyRun(id=" + run.id + ", conversationId=" + run.conversation_id +
           ", parentRunId=" + nullable_text(run.parent_run_id) +
           ", boundaryMessageId=" + nullable_text(run.boundary_message_id) +
           ", status=" + status_name(run.status) +
           ", endReason=" + reason_name(run.end_reason) +
           ", startedAt=" + std::to_string(run.started_at) +
           ", endedAt=" + std::to_string(run.ended_at) +
           ", legacyAmbiguous=" + (run.legacy_ambiguous ? "true" : "false") + ")";
}

std::string describe(const PlannedMessageAssignment& assignment) {
    return "PlannedMessageAssignment(messageId=" + assignment.message_id +
           ", runId=" + assignment.run_id +
           ", runSequence=" + std::to_string(assignment.run_sequence) +
           ", consumedAtPass=" +
           (assignment.consumed_at_pass.has_value()
                ? std::to_string(*assignment.consumed_at_pass)
                : "null") +
           ")";
}

}  // namespace agora::core::legacy_run_backfill
