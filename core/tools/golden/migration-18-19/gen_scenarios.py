#!/usr/bin/env python3
"""Generates Migration18To19 scenarios. Deterministic; rerun after editing.

Each scenario supplies the runs/messages projections the recording database returns. The frozen
trace pins both queries plus every ordered repair UPDATE (with arguments) or exception.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, runs, messages):
    ALL.append(
        {
            "name": name,
            "op": "migrate",
            "runs": runs,
            "messages": messages,
        }
    )


def run(rid, parent=None, legacy=1):
    return {"id": rid, "parentRunId": parent, "legacyAmbiguous": legacy}


def msg(mid, parent, participant, run_id, seq):
    return {
        "id": mid,
        "parentId": parent,
        "participant": participant,
        "runId": run_id,
        "runSequence": seq,
    }


def base(
    name,
    legacy=1,
    canonical_id="u",
    canonical_participant="USER",
    root_parent="u",
    dangling=("d", "gone", 2),
):
    runs = [run("p"), run("r2", "p", legacy)]
    messages = [
        msg(canonical_id, None, canonical_participant, "p", 0),
        msg("root", root_parent, "MODEL", "r2", 1),
    ]
    if dangling is not None:
        messages.append(msg(dangling[0], dangling[1], "MODEL", "r2", dangling[2]))
    case(name, runs, messages)


case("m18_empty", [], [])
case(
    "m18_no_repair",
    [run("p")],
    [msg("u", None, "USER", "p", 0)],
)
base("m18_repair_one")
case(
    "m18_two_updates_sorted",
    [run("p"), run("r2", "p")],
    [
        msg("u", None, "USER", "p", 0),
        msg("root", "u", "MODEL", "r2", 1),
        msg("d2", "gone", "MODEL", "r2", 3),
        msg("d1", "gone", "MODEL", "r2", 2),
    ],
)
base("m18_legacy_zero", legacy=0)
case(
    "m18_parent_missing",
    [run("r2", "px")],
    [
        msg("ux", None, "USER", "px", 0),
        msg("root", "ux", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
case(
    "m18_parent_null",
    [run("r2", None)],
    [
        msg("u", None, "USER", "p", 0),
        msg("root", "u", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
case(
    "m18_no_canonical_user",
    [run("p"), run("r2", "p")],
    [
        msg("m0", None, "MODEL", "p", 0),
        msg("root", "m0", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
case(
    "m18_canonical_user_min_id",
    [run("p"), run("r2", "p")],
    [
        msg("u2", None, "USER", "p", 0),
        msg("u1", None, "USER", "p", 0),
        msg("root", "u1", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
case(
    "m18_canonical_sequence_nonzero",
    [run("p"), run("r2", "p")],
    [
        msg("u", None, "USER", "p", 2),
        msg("root", "u", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
base("m18_no_repaired_root", root_parent="other")
case(
    "m18_repaired_root_tool",
    [run("p"), run("r2", "p")],
    [
        msg("u", None, "USER", "p", 0),
        msg("tool_root", "u", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
base("m18_unknown_participant", canonical_participant="BOGUS")
base("m18_dangling_present_parent", dangling=("d", "root", 2))
base("m18_dangling_tool_prefix", dangling=("tool_d", "gone", 2))
case(
    "m18_duplicate_dangling_id",
    [run("pA"), run("pB"), run("rA", "pA"), run("rB", "pB")],
    [
        msg("uA", None, "USER", "pA", 0),
        msg("uB", None, "USER", "pB", 0),
        msg("rootA", "uA", "MODEL", "rA", 0),
        msg("rootB", "uB", "MODEL", "rB", 0),
        msg("d", "gone", "MODEL", "rA", 1),
        msg("d", "gone", "MODEL", "rB", 1),
    ],
)
case(
    "m18_multiple_runs_order",
    [run("p"), run("r3", "p"), run("r2", "p")],
    [
        msg("u", None, "USER", "p", 0),
        msg("d3", "gone", "MODEL", "r3", 1),
        msg("root3", "u", "MODEL", "r3", 2),
        msg("root2", "u", "MODEL", "r2", 1),
        msg("d2", "gone", "MODEL", "r2", 2),
    ],
)
base("m18_legacy_two", legacy=2)
base("m18_legacy_negative", legacy=-1)
case(
    "m18_self_parent",
    [run("p"), run("r2", "r2")],
    [
        msg("u", None, "USER", "p", 0),
        msg("root", "u", "MODEL", "r2", 1),
        msg("d", "gone", "MODEL", "r2", 2),
    ],
)
case("m18_runs_without_messages", [run("p"), run("r2", "p")], [])


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
