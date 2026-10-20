// Port of data/local/migration/RegenerationTreeRepairPlanner.kt.
//
// Kotlin uses associateBy/groupBy/mapNotNull/linkedMapOf/linkedSetOf/minWithOrNull/maxWithOrNull
// and Json.parseToJsonElement(...).toString(). The port keeps the observable contracts: run lookup
// is last-wins, grouping preserves first appearance, sortedWith is stable, maps/sets emit in
// insertion order with in-place updates for repeated keys.
#include "agora/core/regeneration_tree_repair.hpp"

#include <algorithm>
#include <climits>
#include <cstdint>
#include <string>
#include <string_view>
#include <unordered_map>
#include <unordered_set>

#include "agora/core/json.hpp"
#include "agora/core/sha256.hpp"

namespace agora::core::regeneration_tree_repair {
namespace {

bool starts_with(std::string_view text, std::string_view prefix) {
    return text.size() >= prefix.size() && text.compare(0, prefix.size(), prefix) == 0;
}

bool is_synthetic(const V17MessageRecord& message) {
    return starts_with(message.id, kToolMsgPrefix) || starts_with(message.id, kResultMsgPrefix);
}

bool is_ordinary_user(const V17MessageRecord& message) {
    return message.participant == Participant::User && !is_synthetic(message);
}

bool is_ordinary_model(const V17MessageRecord& message) {
    return message.participant == Participant::Model && !is_synthetic(message);
}

/** compareBy { runSequence }.thenBy { timestamp }.thenBy { id }; app ids are ASCII. */
bool message_less(const V17MessageRecord& left, const V17MessageRecord& right) {
    if (left.run_sequence != right.run_sequence) return left.run_sequence < right.run_sequence;
    if (left.timestamp != right.timestamp) return left.timestamp < right.timestamp;
    return left.id < right.id;
}

template <typename Value>
using OrderedMap = std::vector<std::pair<std::string, Value>>;

template <typename Value>
void set_ordered(OrderedMap<Value>& map, const std::string& key, const Value& value) {
    for (auto& entry : map) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    map.emplace_back(key, value);
}

template <typename Value>
const Value* find_ordered(const OrderedMap<Value>& map, const std::string& key) {
    for (const auto& entry : map) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

/** Kotlin Map<String?, String> iteration order, nullable keys included. */
using NullableKeyMap = std::vector<std::pair<std::optional<std::string>, std::string>>;

void set_nullable(NullableKeyMap& map, const std::optional<std::string>& key,
                  const std::string& value) {
    for (auto& entry : map) {
        if (entry.first == key) {
            entry.second = value;
            return;
        }
    }
    map.emplace_back(key, value);
}

const std::string* find_nullable(const NullableKeyMap& map, const std::optional<std::string>& key) {
    for (const auto& entry : map) {
        if (entry.first == key) return &entry.second;
    }
    return nullptr;
}

void erase_nullable_key(NullableKeyMap& map, const std::optional<std::string>& key) {
    map.erase(std::remove_if(map.begin(), map.end(),
                             [&](const std::pair<std::optional<std::string>, std::string>& entry) {
                                 return entry.first == key;
                             }),
              map.end());
}

struct GroupKey {
    std::optional<std::string> parent_run_id;
    std::optional<std::string> parent_message_id;
    std::string fingerprint;
};

bool operator==(const GroupKey& left, const GroupKey& right) {
    return left.parent_run_id == right.parent_run_id &&
           left.parent_message_id == right.parent_message_id &&
           left.fingerprint == right.fingerprint;
}

struct Group {
    GroupKey key;
    std::vector<std::pair<std::string, const V17MessageRecord*>> entries;
};

struct InferredRepair {
    std::string run_id;
    std::string selected_output_id;
    std::vector<std::string> removed_ids;
};

bool contains_id(const std::vector<std::string>& ids, const std::string& id) {
    return std::find(ids.begin(), ids.end(), id) != ids.end();
}

std::string sha256_hex(std::string_view data) {
    const std::array<std::uint8_t, 32> digest = sha256(data);
    static constexpr char kHex[] = "0123456789abcdef";
    std::string out;
    out.reserve(64);
    for (std::uint8_t byte : digest) {
        out.push_back(kHex[byte >> 4]);
        out.push_back(kHex[byte & 0x0F]);
    }
    return out;
}

json::Value sanitize_attachment(const json::Value& value) {
    if (value.kind == json::Kind::Object) {
        json::Value out;
        out.kind = json::Kind::Object;
        out.begin = value.begin;
        out.end = value.end;
        for (const json::Member& member : value.members) {
            if (member.key == "originalUri") continue;
            out.members.push_back(json::Member{member.key, sanitize_attachment(member.value)});
        }
        return out;
    }
    if (value.kind == json::Kind::Array) {
        json::Value out;
        out.kind = json::Kind::Array;
        out.begin = value.begin;
        out.end = value.end;
        out.elements.reserve(value.elements.size());
        for (const json::Value& element : value.elements) {
            out.elements.push_back(sanitize_attachment(element));
        }
        return out;
    }
    return value;
}

}  // namespace

Plan plan(const std::vector<V17RunRecord>& runs,
          const std::vector<V17MessageRecord>& messages,
          const std::vector<std::pair<std::optional<std::string>, std::string>>& message_selections,
          const std::vector<std::pair<std::optional<std::string>, std::string>>& run_selections) {
    std::unordered_map<std::string, const V17RunRecord*> runs_by_id;
    runs_by_id.reserve(runs.size());
    for (const V17RunRecord& run : runs) runs_by_id[run.id] = &run;

    std::vector<std::string> run_order;
    std::unordered_map<std::string, std::vector<const V17MessageRecord*>> messages_by_run;
    messages_by_run.reserve(runs.size());
    for (const V17MessageRecord& message : messages) {
        auto [it, inserted] = messages_by_run.try_emplace(message.run_id);
        if (inserted) run_order.push_back(message.run_id);
        it->second.push_back(&message);
    }

    std::vector<std::pair<std::string, const V17MessageRecord*>> boundary_by_run;
    std::vector<std::pair<std::string, const V17MessageRecord*>> root_output_by_run;
    for (const std::string& run_id : run_order) {
        const V17MessageRecord* boundary = nullptr;
        const V17MessageRecord* root_output = nullptr;
        for (const V17MessageRecord* message : messages_by_run[run_id]) {
            if (is_ordinary_user(*message) && message->run_sequence == 0 &&
                (boundary == nullptr || message_less(*message, *boundary))) {
                boundary = message;
            }
            if (is_ordinary_model(*message) &&
                (root_output == nullptr || message_less(*message, *root_output))) {
                root_output = message;
            }
        }
        if (boundary != nullptr) boundary_by_run.emplace_back(run_id, boundary);
        if (root_output != nullptr) root_output_by_run.emplace_back(run_id, root_output);
    }

    std::vector<Group> groups;
    for (const auto& [run_id, boundary] : boundary_by_run) {
        GroupKey key;
        const auto run_it = runs_by_id.find(run_id);
        if (run_it != runs_by_id.end()) key.parent_run_id = run_it->second->parent_run_id;
        key.parent_message_id = boundary->parent_id;
        key.fingerprint = boundary->input_fingerprint;

        Group* group = nullptr;
        for (Group& candidate : groups) {
            if (candidate.key == key) {
                group = &candidate;
                break;
            }
        }
        if (group == nullptr) {
            groups.push_back(Group{key, {}});
            group = &groups.back();
        }
        group->entries.emplace_back(run_id, boundary);
    }

    Plan result;
    std::unordered_set<std::string> deleted_ids;
    std::unordered_set<std::string> inferred_ids;
    NullableKeyMap repaired_message_selections(message_selections.begin(),
                                               message_selections.end());
    NullableKeyMap repaired_run_selections(run_selections.begin(), run_selections.end());

    for (const Group& group : groups) {
        if (group.entries.size() < 2) continue;

        std::vector<std::pair<std::string, const V17MessageRecord*>> ordered = group.entries;
        std::stable_sort(
            ordered.begin(), ordered.end(),
            [&](const std::pair<std::string, const V17MessageRecord*>& left,
                const std::pair<std::string, const V17MessageRecord*>& right) {
                const auto left_run = runs_by_id.find(left.first);
                const auto right_run = runs_by_id.find(right.first);
                const long long left_started =
                    left_run == runs_by_id.end() ? LLONG_MAX : left_run->second->started_at;
                const long long right_started =
                    right_run == runs_by_id.end() ? LLONG_MAX : right_run->second->started_at;
                if (left_started != right_started) return left_started < right_started;
                if (left.second->timestamp != right.second->timestamp) {
                    return left.second->timestamp < right.second->timestamp;
                }
                return left.first < right.first;
            });

        const std::string& anchor_run_id = ordered.front().first;
        const V17MessageRecord& anchor_input = *ordered.front().second;
        const auto anchor_it = runs_by_id.find(anchor_run_id);
        if (anchor_it == runs_by_id.end()) continue;
        const V17RunRecord& anchor_run = *anchor_it->second;

        std::vector<InferredRepair> inferred;
        for (std::size_t index = 1; index < ordered.size(); ++index) {
            const std::string& inferred_run_id = ordered[index].first;

            const V17MessageRecord* root_output = nullptr;
            for (const auto& [candidate_id, candidate] : root_output_by_run) {
                if (candidate_id == inferred_run_id) {
                    root_output = candidate;
                    break;
                }
            }
            if (root_output == nullptr) continue;

            const auto messages_it = messages_by_run.find(inferred_run_id);
            if (messages_it == messages_by_run.end()) continue;

            std::vector<const V17MessageRecord*> run_messages = messages_it->second;
            std::stable_sort(run_messages.begin(), run_messages.end(),
                             [](const V17MessageRecord* left, const V17MessageRecord* right) {
                                 return message_less(*left, *right);
                             });

            std::vector<const V17MessageRecord*> leading_inputs;
            for (const V17MessageRecord* message : run_messages) {
                if (message->run_sequence < root_output->run_sequence &&
                    is_ordinary_user(*message)) {
                    leading_inputs.push_back(message);
                }
            }
            if (leading_inputs.empty()) continue;

            InferredRepair repair;
            repair.run_id = inferred_run_id;
            std::unordered_set<std::string> removed;
            for (const V17MessageRecord* input : leading_inputs) {
                repair.removed_ids.push_back(input->id);
                removed.insert(input->id);
            }
            for (const std::string& removed_id : repair.removed_ids) {
                if (deleted_ids.insert(removed_id).second) {
                    result.deleted_message_ids.push_back(removed_id);
                }
            }

            std::vector<const V17MessageRecord*> retained;
            for (const V17MessageRecord* message : run_messages) {
                if (removed.find(message->id) == removed.end()) retained.push_back(message);
            }

            std::vector<const V17MessageRecord*> reparented;
            for (const V17MessageRecord* message : retained) {
                if (!message->parent_id.has_value()) continue;
                if (removed.find(*message->parent_id) == removed.end()) continue;
                reparented.push_back(message);
                set_ordered(result.message_parent_updates, message->id, anchor_input.id);
            }

            set_ordered(result.run_parent_updates, inferred_run_id, anchor_run_id);
            if (inferred_ids.insert(inferred_run_id).second) {
                result.inferred_run_ids.push_back(inferred_run_id);
            }

            for (std::size_t position = 0; position < retained.size(); ++position) {
                set_ordered(result.run_sequence_updates, retained[position]->id,
                            static_cast<long long>(position));
            }

            std::string selected_output_id;
            bool selected_found = false;
            for (auto it = leading_inputs.rbegin(); it != leading_inputs.rend(); ++it) {
                const std::string* selection = nullptr;
                for (const auto& entry : message_selections) {
                    if (entry.first.has_value() && *entry.first == (*it)->id) {
                        selection = &entry.second;
                        break;
                    }
                }
                if (selection == nullptr) continue;
                bool retained_selection = false;
                for (const V17MessageRecord* message : retained) {
                    if (message->id == *selection) {
                        retained_selection = true;
                        break;
                    }
                }
                if (!retained_selection) continue;
                selected_output_id = *selection;
                selected_found = true;
                break;
            }
            if (!selected_found) {
                const V17MessageRecord* latest = nullptr;
                for (const V17MessageRecord* message : reparented) {
                    if (latest == nullptr || message_less(*latest, *message)) latest = message;
                }
                if (latest != nullptr) {
                    selected_output_id = latest->id;
                    selected_found = true;
                }
            }
            if (!selected_found) selected_output_id = root_output->id;
            repair.selected_output_id = selected_output_id;
            inferred.push_back(std::move(repair));
        }
        if (inferred.empty()) continue;

        const std::string* selected_boundary_id =
            find_nullable(repaired_message_selections, anchor_input.parent_id);
        const InferredRepair* selected_by_message = nullptr;
        if (selected_boundary_id != nullptr) {
            for (const InferredRepair& repair : inferred) {
                if (contains_id(repair.removed_ids, *selected_boundary_id)) {
                    selected_by_message = &repair;
                    break;
                }
            }
        }
        const std::string* selected_by_run_id =
            find_nullable(repaired_run_selections, anchor_run.parent_run_id);
        const InferredRepair* selected_by_run = nullptr;
        if (selected_by_run_id != nullptr) {
            for (const InferredRepair& repair : inferred) {
                if (repair.run_id == *selected_by_run_id) {
                    selected_by_run = &repair;
                    break;
                }
            }
        }
        const InferredRepair* selected =
            selected_by_message != nullptr ? selected_by_message : selected_by_run;

        for (const InferredRepair& repair : inferred) {
            for (const std::string& removed_id : repair.removed_ids) {
                erase_nullable_key(repaired_message_selections, removed_id);
            }
        }
        repaired_message_selections.erase(
            std::remove_if(
                repaired_message_selections.begin(), repaired_message_selections.end(),
                [&](const std::pair<std::optional<std::string>, std::string>& entry) {
                    const bool parent_deleted =
                        entry.first.has_value() && deleted_ids.find(*entry.first) != deleted_ids.end();
                    return parent_deleted || deleted_ids.find(entry.second) != deleted_ids.end();
                }),
            repaired_message_selections.end());

        if (selected != nullptr) {
            set_nullable(repaired_message_selections, anchor_input.parent_id, anchor_input.id);
            set_nullable(repaired_message_selections, anchor_input.id,
                         selected->selected_output_id);
            set_nullable(repaired_run_selections, anchor_run.parent_run_id, anchor_run_id);
            set_nullable(repaired_run_selections, anchor_run_id, selected->run_id);
        } else {
            const std::string* current =
                find_nullable(repaired_message_selections, anchor_input.parent_id);
            if (current != nullptr && *current == anchor_input.id) {
                const V17MessageRecord* original_output = nullptr;
                for (const auto& [candidate_id, candidate] : root_output_by_run) {
                    if (candidate_id == anchor_run_id) {
                        original_output = candidate;
                        break;
                    }
                }
                if (original_output != nullptr) {
                    set_nullable(repaired_message_selections, anchor_input.id,
                                 original_output->id);
                    erase_nullable_key(repaired_run_selections, anchor_run_id);
                }
            }
        }
    }

    result.message_selections = std::move(repaired_message_selections);
    result.run_selections = std::move(repaired_run_selections);
    return result;
}

std::string regeneration_input_fingerprint(std::string_view text, int image_count,
                                           const std::optional<std::string>& attachment_meta) {
    std::string normalized;
    if (attachment_meta.has_value() && !json::trim(*attachment_meta).empty()) {
        const json::ParseResult parsed = json::parse(*attachment_meta);
        if (parsed.value.has_value()) {
            normalized = json::to_kotlin_string(sanitize_attachment(*parsed.value));
        } else {
            normalized = *attachment_meta;
        }
    }

    std::string message;
    message.reserve(text.size() + normalized.size() + 16);
    message.append(text);
    message.push_back('\0');
    message += std::to_string(image_count);
    message.push_back('\0');
    message += normalized;
    return sha256_hex(message);
}

}  // namespace agora::core::regeneration_tree_repair
