#!/usr/bin/env python3
"""Generates the V18 regeneration-edge repair scenarios. Deterministic; rerun after editing.

The planner repairs exactly one rejected migration shape: a legacy-ambiguous child Run whose
parent Run exists, whose canonical USER child is still present, and which already has an ordinary
assistant rooted at that user. Only then are ordinary MODEL siblings whose parent row is missing
reparented to the canonical user.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def run(rid, parent=None, legacy=True):
    return {"id": rid, "parentRunId": parent, "legacyAmbiguous": legacy}


def msg(mid, parent, participant, run_id, seq):
    return {
        "id": mid,
        "parentId": parent,
        "participant": participant,
        "runId": run_id,
        "runSequence": seq,
    }


def plan(name, runs, messages):
    case(name, op="plan", runs=runs, messages=messages)


# ---- guard rails ------------------------------------------------------------------------------
plan("plan_empty", [], [])
plan("no_legacy_run", [run("p"), run("r2", "p", legacy=False)], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("legacy_without_parent", [run("r2", None)], [
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("parent_run_missing_but_messages_exist", [run("r2", "px")], [
    msg("ux", None, "USER", "px", 0),
    msg("root", "ux", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("parent_present_no_user", [run("p"), run("r2", "p")], [
    msg("m0", None, "MODEL", "p", 0),
    msg("root", "m0", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("canonical_user_sequence_nonzero", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 2),
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("canonical_user_tool_prefix", [run("p"), run("r2", "p")], [
    msg("tool_u", None, "USER", "p", 0),
    msg("root", "tool_u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("canonical_user_result_prefix", [run("p"), run("r2", "p")], [
    msg("result_u", None, "USER", "p", 0),
    msg("root", "result_u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("no_repaired_root", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("d", "gone", "MODEL", "r2", 2),
])

# ---- the repaired shape -----------------------------------------------------------------------
plan("repaired_root_present", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("sibling_parent_exists", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("m0", "u", "MODEL", "p", 1),
    msg("root", "u", "MODEL", "r2", 1),
    msg("kept", "m0", "MODEL", "r2", 2),
    msg("d", "gone", "MODEL", "r2", 3),
])
plan("sibling_parent_null", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", None, "MODEL", "r2", 2),
])
plan("sibling_tool_prefix", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("tool_d", "gone", "MODEL", "r2", 2),
])
plan("sibling_result_prefix", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("result_d", "gone", "MODEL", "r2", 2),
])
plan("sibling_user", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("u2", "gone", "USER", "r2", 2),
])

# ---- ordering ---------------------------------------------------------------------------------
plan("multiple_siblings_order", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 0),
    msg("d3", "gone", "MODEL", "r2", 3),
    msg("d1", "gone", "MODEL", "r2", 1),
    msg("d2", "gone", "MODEL", "r2", 2),
])
plan("tie_sequence_id_order", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 0),
    msg("d_b", "gone", "MODEL", "r2", 5),
    msg("d_a", "gone", "MODEL", "r2", 5),
])
plan("multiple_runs_order", [
    run("p3"), run("p2"), run("r3", "p3"), run("r2", "p2"),
], [
    msg("u3", None, "USER", "p3", 0),
    msg("u2", None, "USER", "p2", 0),
    msg("root3", "u3", "MODEL", "r3", 0),
    msg("root2", "u2", "MODEL", "r2", 0),
    msg("d3", "gone", "MODEL", "r3", 1),
    msg("d2", "gone", "MODEL", "r2", 1),
])
plan("duplicate_run_ids_last_wins", [
    run("p"), run("r2", "p", legacy=True), run("r2", "p", legacy=False),
], [
    msg("u", None, "USER", "p", 0),
    msg("root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("duplicate_message_id_across_runs", [
    run("pA"), run("pB"), run("rA", "pA"), run("rB", "pB"),
], [
    msg("uA", None, "USER", "pA", 0),
    msg("uB", None, "USER", "pB", 0),
    msg("rootA", "uA", "MODEL", "rA", 0),
    msg("rootB", "uB", "MODEL", "rB", 0),
    msg("d", "gone", "MODEL", "rA", 1),
    msg("d", "gone", "MODEL", "rB", 1),
])

# ---- canonical user / repaired root selection -------------------------------------------------
plan("canonical_user_min_id", [run("p"), run("r2", "p")], [
    msg("u2", None, "USER", "p", 0),
    msg("u1", None, "USER", "p", 0),
    msg("root1", "u1", "MODEL", "r2", 1),
    msg("root2", "u2", "MODEL", "r2", 2),
    msg("d", "gone", "MODEL", "r2", 3),
])
plan("canonical_user_model_ignored", [run("p"), run("r2", "p")], [
    msg("a1", None, "MODEL", "p", 0),
    msg("u9", None, "USER", "p", 0),
    msg("root", "u9", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("repaired_root_tool_prefix", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("tool_root", "u", "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("repaired_root_is_user", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("u2", "u", "USER", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])
plan("repaired_root_parent_null", [run("p"), run("r2", "p")], [
    msg("u", None, "USER", "p", 0),
    msg("root", None, "MODEL", "r2", 1),
    msg("d", "gone", "MODEL", "r2", 2),
])

# ---- constants --------------------------------------------------------------------------------
case("constants", op="constants")


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for existing in os.listdir(SCENARIOS_DIR):
        if existing.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, existing))
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")


if __name__ == "__main__":
    main()
