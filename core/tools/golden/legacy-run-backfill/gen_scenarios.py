#!/usr/bin/env python3
"""Generates the legacy-run backfill scenarios. Deterministic; rerun after editing.

`plan` pins the v16 message forest -> v17 Run conversion: every visible USER starts a Run,
descendants keep the parent Run until the next visible USER, orphans and cycles seed ambiguous
orphan Runs, deterministic UUID v3 Run ids, topology order and the status/end-reason derivation.
`selected` pins the legacy message-selection -> Run-selection migration.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def msg(mid, parent, participant, status, timestamp, synthetic=None):
    fields = {
        "id": mid,
        "parentId": parent,
        "participant": participant,
        "status": status,
        "timestamp": timestamp,
    }
    if synthetic is not None:
        fields["isSynthetic"] = synthetic
    return fields


def plan(name, conv, messages):
    case(name, op="plan", conversationId=conv, messages=messages)


def selected(name, conv, messages, selections=None):
    fields = {"op": "selected", "conversationId": conv, "messages": messages}
    if selections is not None:
        fields["selectedMessageBranches"] = selections
    case(name, **fields)


def user(mid, parent, ts, status="SUCCESS", synthetic=None):
    return msg(mid, parent, "USER", status, ts, synthetic)


def model(mid, parent, ts, status="SUCCESS", synthetic=None):
    return msg(mid, parent, "MODEL", status, ts, synthetic)


# ---- plan: shape ------------------------------------------------------------------------------
plan("plan_empty", "c", [])
plan("plan_single_user", "c", [user("u", None, 10)])
plan("plan_user_model", "c", [
    user("u", None, 10),
    model("m", "u", 11),
])
plan("plan_two_boundaries", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2", "m1", 12),
    model("m2", "u2", 13),
])
plan("plan_orphan_model", "c", [model("m", "x", 10)])
plan("plan_synthetic_user", "c", [user("tool_x", None, 10)])
plan("plan_tool_chain", "c", [
    user("u", None, 10),
    msg("tool_t", "u", "MODEL", "SUCCESS", 11),
    msg("result_r", "tool_t", "MODEL", "SUCCESS", 12),
])
plan("plan_nested_boundary_under_tool", "c", [
    user("u", None, 10),
    msg("tool_t", "u", "MODEL", "SUCCESS", 11),
    user("u2", "tool_t", 12),
])
plan("plan_multiple_roots", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2", None, 20),
])
plan("plan_deep_chain", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2", "m1", 12),
    model("m2", "u2", 13),
    user("u3", "m2", 14),
    model("m3", "u3", 15),
])
plan("plan_child_before_parent_input", "c", [
    model("m", "u", 11),
    user("u", None, 10),
])
plan("plan_tie_children_order", "c", [
    user("u", None, 10),
    model("m_b", "u", 11),
    model("m_a", "u", 11),
])
plan("plan_synthetic_flag_override", "c", [
    user("u", None, 10),
    model("plain_model", "u", 11, synthetic=True),
])
plan("plan_user_marked_synthetic", "c", [
    user("u_x", None, 10, synthetic=True),
])

# ---- plan: status derivation ------------------------------------------------------------------
plan("plan_error_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="ERROR"),
])
plan("plan_error_participant", "c", [
    user("u", None, 10),
    msg("e", "u", "ERROR", "SUCCESS", 11),
])
plan("plan_stopped_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="STOPPED"),
])
plan("plan_transcribing_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="TRANSCRIBING"),
])
plan("plan_sending_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="SENDING"),
])
plan("plan_thinking_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="THINKING"),
])
plan("plan_tool_calling_status", "c", [
    user("u", None, 10),
    model("m", "u", 11, status="TOOL_CALLING"),
])
plan("plan_error_beats_stopped", "c", [
    user("u", None, 10),
    model("m1", "u", 11, status="STOPPED"),
    model("m2", "u", 12, status="ERROR"),
])

# ---- plan: ambiguity --------------------------------------------------------------------------
plan("plan_ambiguous_model_siblings", "c", [
    user("u", None, 10),
    model("m1", "u", 11),
    model("m2", "u", 12),
])
plan("plan_synthetic_siblings", "c", [
    user("u", None, 10),
    msg("tool_a", "u", "MODEL", "SUCCESS", 11),
    msg("result_b", "u", "MODEL", "SUCCESS", 12),
])
plan("plan_mixed_siblings", "c", [
    user("u", None, 10),
    model("m1", "u", 11),
    msg("tool_a", "u", "MODEL", "SUCCESS", 12),
])

# ---- plan: corrupt topology / requires --------------------------------------------------------
plan("plan_orphan_parent_user", "c", [
    user("u", "gone", 10),
    model("m", "u", 11),
])
plan("plan_cycle_input_order", "c", [
    model("z", "y", 30),
    model("y", "x", 20),
    model("x", "z", 10),
])
plan("plan_cycle_two", "c", [
    model("a", "b", 10),
    model("b", "a", 11),
])
plan("plan_cycle_three", "c", [
    model("a", "b", 10),
    model("b", "c", 11),
    model("c", "a", 12),
])
plan("plan_duplicate_ids", "c", [
    user("u", None, 10),
    model("u", "u", 11),
])
plan("plan_blank_conversation", "   ", [user("u", None, 10)])
plan("plan_empty_conversation", "", [user("u", None, 10)])

# ---- selected ---------------------------------------------------------------------------------
selected("selected_empty", "c", [])
selected("selected_roots", "c", [
    user("u1", None, 10),
    user("u2", None, 20),
], {"null": "u1"})
selected("selected_roots_default", "c", [
    user("u1", None, 10),
    user("u2", None, 20),
])
selected("selected_roots_missing", "c", [
    user("u1", None, 10),
    user("u2", None, 20),
], {"null": "ghost"})
selected("selected_roots_synthetic", "c", [
    user("tool_x", None, 5),
    user("u2", None, 20),
], {"null": "tool_x"})
selected("selected_run_chain", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2a", "m1", 12),
    user("u2b", "m1", 13),
    model("m2a", "u2a", 14),
    model("m2b", "u2b", 15),
], {"m1": "u2a"})
selected("selected_run_chain_default", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2a", "m1", 12),
    user("u2b", "m1", 13),
    model("m2a", "u2a", 14),
    model("m2b", "u2b", 15),
])
selected("selected_run_chain_missing", "c", [
    user("u1", None, 10),
    model("m1", "u1", 11),
    user("u2a", "m1", 12),
    user("u2b", "m1", 13),
    model("m2a", "u2a", 14),
    model("m2b", "u2b", 15),
], {"m1": "ghost"})
selected("selected_boundary_same_run", "c", [
    user("u", None, 10),
    model("m1", "u", 11),
    model("m2", "u", 12),
], {"u": "m1"})
selected("selected_cycle", "c", [
    model("a", "b", 10),
    model("b", "a", 11),
])


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
