#include "agora/core/custom_model_references.hpp"

#include <cstddef>
#include <string>
#include <unordered_map>
#include <unordered_set>

#include "agora/core/json.hpp"

namespace agora::core::custom_model_references {
namespace {

/** LinkedHashSet.add: a duplicate keeps its original position. */
void set_add(OrderedSet& set, std::unordered_set<std::string>& seen, const std::string& value) {
    if (seen.insert(value).second) set.push_back(value);
}

}  // namespace

OrderedSet replace_model_reference(const OrderedSet& model_ids,
                                   std::string_view old_model_id,
                                   std::optional<std::string> new_model_id) {
    OrderedSet result;
    result.reserve(model_ids.size());
    std::unordered_set<std::string> seen;
    seen.reserve(model_ids.size());
    for (const std::string& model_id : model_ids) {
        if (model_id != old_model_id) {
            set_add(result, seen, model_id);
        } else if (new_model_id.has_value()) {
            set_add(result, seen, *new_model_id);
        }
    }
    return result;
}

std::optional<std::string> replace_model_reference(std::optional<std::string> value,
                                                   std::string_view old_model_id,
                                                   std::optional<std::string> new_model_id) {
    if (value.has_value() && *value == old_model_id) return new_model_id;
    return value;
}

OrderedMap replace_custom_model_alias(const OrderedMap& aliases,
                                      std::string_view old_model_id,
                                      std::optional<std::string> new_model_id,
                                      std::string_view alias) {
    // toMutableMap(): source iteration order, a repeated key keeps its first position and takes
    // the last value.
    OrderedMap result;
    result.reserve(aliases.size() + 1);
    std::unordered_map<std::string, std::size_t> positions;
    for (const auto& [key, value] : aliases) {
        const auto found = positions.find(key);
        if (found == positions.end()) {
            positions.emplace(key, result.size());
            result.emplace_back(key, value);
        } else {
            result[found->second].second = value;
        }
    }

    // remove(oldModelId)
    for (auto it = result.begin(); it != result.end(); ++it) {
        if (it->first == old_model_id) {
            result.erase(it);
            break;
        }
    }

    // if (newModelId != null && alias.isNotBlank()) this[newModelId] = alias.trim()
    if (new_model_id.has_value() && !json::is_blank(alias)) {
        const std::string trimmed = json::trim(alias);
        bool replaced = false;
        for (auto& entry : result) {
            if (entry.first == *new_model_id) {
                entry.second = trimmed;
                replaced = true;
                break;
            }
        }
        if (!replaced) result.emplace_back(*new_model_id, trimmed);
    }
    return result;
}

}  // namespace agora::core::custom_model_references
