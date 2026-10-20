// Port of data/local/migration/V18RegenerationEdgeRepairPlanner.kt.
//
// Kotlin uses associateBy/groupBy/mapTo/sortedWith/minWithOrNull/linkedMapOf. The port keeps the
// observable contracts of those calls: last run id wins in the lookup map, grouping preserves
// input order, the canonical user is the minimum by (runSequence, id), and the repair map is
// insertion-ordered with in-place overwrite of repeated keys.
#include "agora/core/v18_regeneration_repair.hpp"

#include <algorithm>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

namespace agora::core::v18_regeneration_repair {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_ordinary(const MessageRecord& message) {
    return !starts_with(message.id, kToolMsgPrefix) && !starts_with(message.id, kResultMsgPrefix);
}

/** compareBy { runSequence }.thenBy { id }; app ids are ASCII, so byte order matches the JVM. */
bool message_less(const MessageRecord& left, const MessageRecord& right) {
    if (left.run_sequence != right.run_sequence) return left.run_sequence < right.run_sequence;
    return left.id < right.id;
}

}  // namespace

std::vector<std::pair<std::string, std::string>> plan(const std::vector<RunRecord>& runs,
                                                      const std::vector<MessageRecord>& messages) {
    std::unordered_map<std::string, const RunRecord*> runs_by_id;
    runs_by_id.reserve(runs.size());
    for (const RunRecord& run : runs) runs_by_id[run.id] = &run;

    std::unordered_map<std::string, std::vector<const MessageRecord*>> messages_by_run;
    messages_by_run.reserve(runs.size());
    for (const MessageRecord& message : messages) {
        messages_by_run[message.run_id].push_back(&message);
    }

    std::unordered_set<std::string> message_ids;
    message_ids.reserve(messages.size());
    for (const MessageRecord& message : messages) message_ids.insert(message.id);

    std::vector<std::pair<std::string, std::string>> updates;

    for (const RunRecord& run : runs) {
        if (!run.legacy_ambiguous) continue;
        if (!run.parent_run_id.has_value()) continue;
        const std::string& parent_run_id = *run.parent_run_id;
        if (runs_by_id.find(parent_run_id) == runs_by_id.end()) continue;

        const MessageRecord* canonical_user = nullptr;
        const auto parent_messages = messages_by_run.find(parent_run_id);
        if (parent_messages != messages_by_run.end()) {
            for (const MessageRecord* candidate : parent_messages->second) {
                if (!is_ordinary(*candidate)) continue;
                if (candidate->participant != Participant::User) continue;
                if (candidate->run_sequence != 0) continue;
                if (canonical_user == nullptr || message_less(*candidate, *canonical_user)) {
                    canonical_user = candidate;
                }
            }
        }
        if (canonical_user == nullptr) continue;

        const auto run_messages = messages_by_run.find(run.id);
        if (run_messages == messages_by_run.end()) continue;

        bool has_repaired_root = false;
        for (const MessageRecord* message : run_messages->second) {
            if (is_ordinary(*message) && message->participant == Participant::Model &&
                message->parent_id.has_value() && *message->parent_id == canonical_user->id) {
                has_repaired_root = true;
                break;
            }
        }
        if (!has_repaired_root) continue;

        std::vector<const MessageRecord*> dangling;
        for (const MessageRecord* message : run_messages->second) {
            if (!is_ordinary(*message)) continue;
            if (message->participant != Participant::Model) continue;
            if (!message->parent_id.has_value()) continue;
            if (message_ids.find(*message->parent_id) != message_ids.end()) continue;
            dangling.push_back(message);
        }
        std::sort(dangling.begin(), dangling.end(),
                  [](const MessageRecord* left, const MessageRecord* right) {
                      return message_less(*left, *right);
                  });

        for (const MessageRecord* message : dangling) {
            // Kotlin linkedMapOf: a repeated id keeps its first position and takes the new value.
            auto existing = std::find_if(
                updates.begin(), updates.end(),
                [&](const std::pair<std::string, std::string>& entry) {
                    return entry.first == message->id;
                });
            if (existing == updates.end()) {
                updates.emplace_back(message->id, canonical_user->id);
            } else {
                existing->second = canonical_user->id;
            }
        }
    }
    return updates;
}

}  // namespace agora::core::v18_regeneration_repair
