#!/usr/bin/env python3
"""Generates the run-branch-selection scenarios. Deterministic; rerun after editing.

The policy validates a durable parent-Run -> selected-child-Run projection against the Run tree.
The interesting inputs are the edges that can look plausible but are not in the tree: a missing
child, a missing parent, a root selection pointing at a child that has a parent, a self-edge, and
duplicate run ids (associateBy keeps the last one). Selection order is observable and pinned.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, selections, runs):
    ALL.append({
        "name": name,
        "op": "retainValidEdges",
        "selections": selections,
        "runs": [{"id": run_id, "parentRunId": parent} for run_id, parent in runs],
    })


def run(run_id, parent=None):
    return (run_id, parent)


# ---- the happy paths ------------------------------------------------------------------------
case("keeps_valid_edge", [["p", "c"]], [run("p"), run("c", "p")])
case("keeps_root_child", [[None, "c"]], [run("c")])
case("chain_keeps_every_valid_edge",
     [[None, "root"], ["root", "a"], ["a", "b"]],
     [run("root"), run("a", "root"), run("b", "a")])

# ---- the child side -------------------------------------------------------------------------
case("drops_missing_child", [["p", "c"]], [run("p")])
case("drops_root_selection_for_missing_child", [[None, "c"]], [])
case("drops_root_child_that_has_a_parent", [[None, "c"]], [run("c", "p")])
case("drops_non_root_selection_for_root_child", [["p", "c"]], [run("p"), run("c")])
case("drops_child_with_another_parent",
     [["p", "c"]], [run("p"), run("q"), run("c", "q")])
case("child_repeated_under_two_parents",
     [["p", "c"], ["q", "c"]], [run("p"), run("q"), run("c", "p")])

# ---- the parent side ------------------------------------------------------------------------
case("drops_missing_parent", [["p", "c"]], [run("c", "p")])
case("drops_self_edge", [["c", "c"]], [run("c", "c")])
case("drops_self_edge_for_root_child", [["c", "c"]], [run("c")])

# ---- containers -----------------------------------------------------------------------------
case("empty_selections", [], [run("a")])
case("empty_runs", [["p", "c"]], [])
case("preserves_selection_order",
     [["p1", "c1"], [None, "c3"], ["p2", "c2"]],
     [run("p1"), run("c1", "p1"), run("p2"), run("c2", "p2"), run("c3")])
case("keeps_valid_and_drops_invalid_in_order",
     [[None, "root"], ["p", "c"], ["q", "c"], [None, "missing"]],
     [run("root"), run("p"), run("q"), run("c", "p")])

# associateBy keeps the last entity for a duplicated id.
case("duplicate_run_ids_last_wins",
     [["p1", "c"], ["p2", "c"]],
     [run("p1"), run("p2"), run("c", "p1"), run("c", "p2")])
case("duplicate_run_ids_last_is_root",
     [[None, "c"]], [run("c", "p"), run("c")])
case("duplicate_run_ids_last_has_parent",
     [[None, "c"]], [run("c"), run("c", "p")])
case("duplicate_run_ids_last_root_drops_parent_edge",
     [[None, "x"], ["p2", "x"]],
     [run("x", "p1"), run("x", "p2"), run("x")])

# The selection map itself is a map: a repeated key keeps its first position and its last value.
case("duplicate_selection_key_last_value_wins",
     [["p1", "c1"], ["p1", "c2"]],
     [run("p1"), run("c1", "p1"), run("c2", "p1")])
case("duplicate_null_key_last_value_wins",
     [[None, "c1"], [None, "c2"]],
     [run("c1"), run("c2")])

# ---- empty strings are real ids, and null is not the empty string ---------------------------
case("empty_string_parent_id",
     [["", "c"]], [run(""), run("c", "")])
case("empty_string_child_id",
     [["p", ""]], [run("p"), run("", "p")])
case("null_parent_is_not_empty_string",
     [[None, "c"]], [run("c", "")])

# ---- escaping -------------------------------------------------------------------------------
case("separators_are_escaped",
     [["a=b|c;d", "c"]], [run("a=b|c;d"), run("c", "a=b|c;d")])

# ---- unrelated runs -------------------------------------------------------------------------
case("unrelated_runs_are_ignored",
     [["p", "c"]], [run("x"), run("y", "x"), run("p"), run("c", "p")])

os.makedirs(SCENARIOS_DIR, exist_ok=True)
for stale in os.listdir(SCENARIOS_DIR):
    if stale.endswith(".json"):
        os.remove(os.path.join(SCENARIOS_DIR, stale))
for entry in ALL:
    path = os.path.join(SCENARIOS_DIR, entry["name"] + ".json")
    with open(path, "w", encoding="utf-8") as handle:
        json.dump(entry, handle, ensure_ascii=False)
        handle.write("\n")
print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")
