#include "agora/core/run_branch_selection.hpp"

#include "agora/core/json.hpp"

#include <optional>
#include <string>
#include <unordered_map>
#include <vector>

namespace agora::core::run_branch_selection {

std::vector<Selection> retain_valid_edges(const std::vector<Selection>& selections,
                                          const std::vector<RunRef>& runs) {
    if (selections.empty()) return {};

    // associateBy(RunEntity::id): a duplicated id keeps the last entity.
    std::unordered_map<std::string, const RunRef*> runs_by_id;
    runs_by_id.reserve(runs.size());
    for (const RunRef& run : runs) runs_by_id[run.id] = &run;

    std::vector<Selection> kept;
    kept.reserve(selections.size());
    for (const Selection& selection : selections) {
        const auto child_entry = runs_by_id.find(selection.child_run_id);
        if (child_entry == runs_by_id.end()) continue;  // the child Run is gone
        const RunRef& child = *child_entry->second;

        if (!selection.parent_run_id.has_value()) {
            // No explicit parent: only a root child matches.
            if (!child.parent_run_id.has_value()) kept.push_back(selection);
            continue;
        }

        // `parentRunId != childRunId && parentRunId in runsById && child.parentRunId == parentRunId`
        if (*selection.parent_run_id == selection.child_run_id) continue;
        if (runs_by_id.find(*selection.parent_run_id) == runs_by_id.end()) continue;
        if (child.parent_run_id != selection.parent_run_id) continue;
        kept.push_back(selection);
    }
    return kept;
}

std::optional<SelectionMap> decode_selection_map(const std::string& raw) {
    const json::ParseResult parsed = json::parse(raw);
    if (!parsed.value.has_value() || parsed.value->kind != json::Kind::Object) return std::nullopt;
    SelectionMap selections;
    for (const json::Member& member : parsed.value->members) {
        if (member.value.kind != json::Kind::String) return std::nullopt;
        const std::optional<std::string> key =
            member.key == "null" ? std::nullopt : std::optional<std::string>(member.key);
        bool replaced = false;
        for (auto& entry : selections) {
            if (entry.first == key) {
                entry.second = member.value.text;
                replaced = true;
                break;
            }
        }
        if (!replaced) selections.emplace_back(key, member.value.text);
    }
    return selections;
}

std::string encode_selection_map(const SelectionMap& selections) {
    json::Value object;
    object.kind = json::Kind::Object;
    for (const auto& [key, value] : selections) {
        json::Member member;
        member.key = key.value_or("null");
        member.value.kind = json::Kind::String;
        member.value.text = value;
        object.members.push_back(std::move(member));
    }
    return json::to_kotlin_string(object);
}

}  // namespace agora::core::run_branch_selection
