#pragma once

#include <optional>
#include <string>
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

}  // namespace agora::core::run_branch_selection
