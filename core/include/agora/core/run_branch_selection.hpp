#pragma once

#include <optional>
#include <string>
#include <utility>
#include <vector>

namespace agora::core::run_branch_selection {

/**
 * Port of data/local/RunBranchSelectionIntegrity.kt: validation of the durable parent-Run ->
 * selected-child-Run projection against the Run tree.
 *
 * The projection is only a selection: dropping an invalid entry never deletes a Run or a message.
 * With no explicit selection the caller's deterministic branch fallback applies, which is why an
 * empty input must stay empty rather than "repair" itself into something.
 */

/**
 * The two RunEntity columns the policy reads. The other columns (status, timestamps, pass,
 * terminal reason) are never consulted here; the stand builds full entities to produce the golden
 * traces, and this port stays at the boundary the function actually touches.
 */
struct RunRef {
    std::string id;
    std::optional<std::string> parent_run_id;
};

/** One durable selection: a nullable parent Run id mapped to the selected child Run id. */
struct Selection {
    std::optional<std::string> parent_run_id;
    std::string child_run_id;
};

/**
 * Keeps only the selections that agree with the tree, in input order.
 *
 * `associateBy(RunEntity::id)` keeps the **last** entity for a duplicated id, so a run list with
 * repeated ids resolves to its final entry. A selection is kept when the child exists and either
 * the parent key is null and the child is a root, or the parent key names an existing Run, is not
 * the child itself, and the child's `parentRunId` matches it.
 */
std::vector<Selection> retain_valid_edges(const std::vector<Selection>& selections,
                                          const std::vector<RunRef>& runs);

/**
 * LinkedHashMap<String?, String>: input order, the last value wins at the first position.
 * The nullable key is how Kotlin's `mapKeys { if (it.key == "null") null else it.key }` round trip
 * represents a null parent Run id in JSON.
 */
using SelectionMap = std::vector<std::pair<std::optional<std::string>, std::string>>;

/**
 * decodeSelectionMap: `Json.decodeFromString<Map<String, String>>(raw)` inside runCatching —
 * null when the raw text is not a JSON object of string values.
 */
std::optional<SelectionMap> decode_selection_map(const std::string& raw);

/** encodeSelectionMap: `Json.encodeToString(selections.mapKeys { it.key ?: "null" })`. */
std::string encode_selection_map(const SelectionMap& selections);

}  // namespace agora::core::run_branch_selection
