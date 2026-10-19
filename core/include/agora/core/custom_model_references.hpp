#pragma once

#include <optional>
#include <string>
#include <string_view>
#include <utility>
#include <vector>

namespace agora::core::custom_model_references {

/**
 * Port of data/CustomModelReferences.kt: the rename/delete rewriting applied to persisted model
 * selections when a custom model is edited or removed.
 *
 * The Kotlin file is three extension functions with no imports and no state. The only thing that
 * needs care is that the containers are ordered: a `Set` literal in Kotlin is a `LinkedHashSet`
 * and `toMutableMap()` is a `LinkedHashMap`, so an identifier's position is observable and part of
 * the contract. The values here carry that order explicitly.
 */

/** A set with insertion order: a value's first occurrence fixes its position. */
using OrderedSet = std::vector<std::string>;

/** A map with insertion order; duplicated keys keep the first position and the last value. */
using OrderedMap = std::vector<std::pair<std::string, std::string>>;

/** `Set<String>.replaceModelReference`: rewrite every occurrence of the old id, dropping the old. */
OrderedSet replace_model_reference(const OrderedSet& model_ids,
                                   std::string_view old_model_id,
                                   std::optional<std::string> new_model_id);

/** `String?.replaceModelReference`: rewrite the value only when it equals the old id. */
std::optional<std::string> replace_model_reference(std::optional<std::string> value,
                                                   std::string_view old_model_id,
                                                   std::optional<std::string> new_model_id);

/**
 * `Map<String, String>.replaceCustomModelAlias`: drop the old entry, then store the trimmed alias
 * under the new id unless there is no new id or the alias is blank.
 *
 * Note the Kotlin order of operations: `remove(old)` precedes the insert, so renaming an entry to
 * its own id moves it to the end, and a target that already exists keeps its position while taking
 * the new value.
 */
OrderedMap replace_custom_model_alias(const OrderedMap& aliases,
                                      std::string_view old_model_id,
                                      std::optional<std::string> new_model_id,
                                      std::string_view alias);

}  // namespace agora::core::custom_model_references
