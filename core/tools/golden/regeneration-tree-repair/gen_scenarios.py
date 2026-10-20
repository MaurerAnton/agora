#!/usr/bin/env python3
"""Generates the regeneration-tree repair scenarios. Deterministic; rerun after editing.

`plan` pins the v17 Regenerate/Edit discriminator: sibling Runs with the same parent Run, parent
message and exact input fingerprint are one group; the earliest is the anchor and later Runs are
inferred regenerations whose leading cloned user inputs are deleted and whose root assistant is
reparented to the anchor user. `fingerprint` pins the attachment sanitization + SHA-256 used as
the fingerprint itself.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def run(rid, parent=None, started=0):
    return {"id": rid, "parentRunId": parent, "startedAt": started}


def msg(mid, parent, participant, timestamp, run_id, seq, fp="fpA"):
    return {
        "id": mid,
        "parentId": parent,
        "participant": participant,
        "timestamp": timestamp,
        "runId": run_id,
        "runSequence": seq,
        "inputFingerprint": fp,
    }


def plan(name, runs, messages, message_selections=None, run_selections=None):
    fields = {"op": "plan", "runs": runs, "messages": messages}
    if message_selections is not None:
        fields["messageSelections"] = message_selections
    if run_selections is not None:
        fields["runSelections"] = run_selections
    case(name, **fields)


def fingerprint(name, text, image_count=0, attachment_meta=None):
    fields = {"op": "fingerprint", "text": text, "imageCount": image_count}
    if attachment_meta is not None:
        fields["attachmentMeta"] = attachment_meta
    case(name, **fields)


# ---- plan: guard rails ------------------------------------------------------------------------
plan("plan_empty", [], [])
plan("single_run_no_group", [run("r1", started=100)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
])
plan("distinct_fingerprints", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0, "fpA"),
    msg("m1", "u1", "MODEL", 11, "r1", 1, "fpA"),
    msg("u2", "p0", "USER", 12, "r2", 0, "fpB"),
    msg("m2", "u2", "MODEL", 13, "r2", 1, "fpB"),
])
plan("synthetic_boundary_ignored", [run("r1", started=100), run("r2", started=200)], [
    msg("tool_u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "tool_u1", "MODEL", 11, "r1", 1),
    msg("result_u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "result_u2", "MODEL", 13, "r2", 1),
])
plan("error_rows_ignored", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("e2", "p0", "ERROR", 12, "r2", 0),
    msg("m2", "e2", "MODEL", 13, "r2", 1),
])

# ---- plan: the regenerate shape ---------------------------------------------------------------
plan("regenerate_pair", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])
plan("reverse_started_at", [run("r1", started=200), run("r2", started=100)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])
plan("tie_started_at_run_id", [run("r1", started=100), run("r2", started=100)], [
    msg("u2", "p0", "USER", 10, "r2", 0),
    msg("m2", "u2", "MODEL", 11, "r2", 1),
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
])
plan("tie_started_at_timestamp", [run("r1", started=100), run("r2", started=100)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 5, "r2", 0),
    msg("m2", "u2", "MODEL", 6, "r2", 1),
])
plan("missing_run_record", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
    msg("ug", "p0", "USER", 12, "rG", 0),
    msg("mg", "ug", "MODEL", 13, "rG", 1),
])
plan("all_runs_missing", [], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])
plan("inferred_without_root", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
])
plan("no_leading_inputs", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 12, "r2", 0),
])
plan("three_runs_same_input", [run("r1", started=100), run("r2", started=200), run("r3", started=300)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
    msg("u3", "p0", "USER", 14, "r3", 0),
    msg("m3", "u3", "MODEL", 15, "r3", 1),
])
plan("duplicate_run_ids_last_wins", [
    run("r1", started=300), run("r2", started=200), run("r1", started=100),
], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])
plan("duplicate_boundary_user", [run("r1", started=100), run("r2", started=200)], [
    msg("u1b", "p0", "USER", 9, "r1", 0),
    msg("u1a", "p0", "USER", 5, "r1", 0),
    msg("m1", "u1a", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 7, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])

# ---- plan: selections -------------------------------------------------------------------------
plan("boundary_null_parent_message_selection",
     [run("r1", started=100), run("r2", started=200)], [
         msg("u1", None, "USER", 10, "r1", 0),
         msg("m1", "u1", "MODEL", 11, "r1", 1),
         msg("u2", None, "USER", 12, "r2", 0),
         msg("m2", "u2", "MODEL", 13, "r2", 1),
     ], message_selections={"null": "u2"})
plan("boundary_null_parent_run_selection",
     [run("r1", started=100), run("r2", started=200)], [
         msg("u1", None, "USER", 10, "r1", 0),
         msg("m1", "u1", "MODEL", 11, "r1", 1),
         msg("u2", None, "USER", 12, "r2", 0),
         msg("m2", "u2", "MODEL", 13, "r2", 1),
     ], run_selections={"null": "r2"})
plan("run_selection_parent_run", [run("r1", "pr", 100), run("r2", "pr", 200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
], run_selections={"pr": "r2"})
plan("selection_missing_retained", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
], message_selections={"p0": "gone"})
plan("selection_not_removed", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
], message_selections={"p0": "m1"})
plan("self_edge_branch", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
], message_selections={"p0": "u1"}, run_selections={"r1": "r1", "r2": "r2"})
plan("two_leading_inputs", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2a", "p0", "USER", 12, "r2", 0),
    msg("u2b", "p0", "USER", 13, "r2", 1),
    msg("m2", "u2b", "MODEL", 14, "r2", 2),
])
plan("two_leading_inputs_selected", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2a", "p0", "USER", 12, "r2", 0),
    msg("u2b", "p0", "USER", 13, "r2", 1),
    msg("m2a", "u1", "MODEL", 14, "r2", 2),
    msg("m2b", "u1", "MODEL", 15, "r2", 3),
], message_selections={"p0": "u2a", "u2a": "m2a", "u2b": "m2b"})
plan("selection_output_removed_fallback", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2a", "p0", "USER", 12, "r2", 0),
    msg("u2b", "p0", "USER", 13, "r2", 1),
    msg("m2a", "u1", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2a", "u2a": "u2b"})
plan("selected_root_fallback", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2a", "p0", "USER", 12, "r2", 0),
    msg("m2a", "u1", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2a"})
plan("boundary_nonzero_user", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 1),
    msg("m1", "u1", "MODEL", 11, "r1", 2),
    msg("u2", "p0", "USER", 12, "r2", 1),
    msg("m2", "u2", "MODEL", 13, "r2", 2),
])
plan("synthetic_boundary_with_leading", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("tool_x", "p0", "USER", 12, "r2", 0),
    msg("u2", "p0", "USER", 13, "r2", 1),
    msg("m2", "u2", "MODEL", 14, "r2", 2),
])
plan("selected_max_reparented", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2a", "u2", "MODEL", 13, "r2", 1),
    msg("m2b", "u2", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2"})
plan("synthetic_root_candidate", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("tool_m", "u1", "MODEL", 13, "r2", 1),
    msg("m2", "u1", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2"})
plan("error_root_candidate", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("e2", "u1", "ERROR", 13, "r2", 1),
    msg("m2", "u1", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2"})
plan("model_leading_candidate", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("x0", "u1", "MODEL", 13, "r2", 1),
    msg("m2", "u2", "MODEL", 14, "r2", 2),
], message_selections={"p0": "u2"})
plan("user_after_root", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("v2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u1", "MODEL", 13, "r2", 0),
    msg("u2", "p0", "USER", 14, "r2", 1),
])
plan("different_boundary_parents", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "pA", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "pB", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
])
plan("three_runs_both_selections", [
    run("r1", started=100), run("r2", started=200), run("r3", started=300),
], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
    msg("u3", "p0", "USER", 14, "r3", 0),
    msg("m3", "u3", "MODEL", 15, "r3", 1),
], message_selections={"p0": "u2"}, run_selections={"null": "r3"})
plan("synthetic_reparented", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
    msg("tool_h", "u2", "MODEL", 14, "r2", 2),
])
plan("selections_cleanup", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 12, "r2", 0),
    msg("m2", "u2", "MODEL", 13, "r2", 1),
], message_selections={"u2": "keep", "x": "u2", "p0": "u2", "y": "z"})
plan("sequence_by_timestamp", [run("r1", started=100), run("r2", started=200)], [
    msg("u1", "p0", "USER", 10, "r1", 0),
    msg("m1", "u1", "MODEL", 11, "r1", 1),
    msg("u2", "p0", "USER", 50, "r2", 0),
    msg("m2", "u2", "MODEL", 20, "r2", 10),
    msg("m2b", "u1", "MODEL", 10, "r2", 10),
])
plan("two_groups", [
    run("r1", started=100), run("r3", started=110),
    run("r2", started=200), run("r4", started=210),
], [
    msg("u1", "p0", "USER", 10, "r1", 0, "fpA"),
    msg("m1", "u1", "MODEL", 11, "r1", 1, "fpA"),
    msg("u3", "pB", "USER", 12, "r3", 0, "fpB"),
    msg("m3", "u3", "MODEL", 13, "r3", 1, "fpB"),
    msg("u2", "p0", "USER", 14, "r2", 0, "fpA"),
    msg("m2", "u2", "MODEL", 15, "r2", 1, "fpA"),
    msg("u4", "pB", "USER", 16, "r4", 0, "fpB"),
    msg("m4", "u4", "MODEL", 17, "r4", 1, "fpB"),
])
plan("two_groups_selected", [
    run("r1", started=100), run("r3", started=110),
    run("r2", started=200), run("r4", started=210),
], [
    msg("u1", "p0", "USER", 10, "r1", 0, "fpA"),
    msg("m1", "u1", "MODEL", 11, "r1", 1, "fpA"),
    msg("u3", "pB", "USER", 12, "r3", 0, "fpB"),
    msg("m3", "u3", "MODEL", 13, "r3", 1, "fpB"),
    msg("u2", "p0", "USER", 14, "r2", 0, "fpA"),
    msg("m2", "u2", "MODEL", 15, "r2", 1, "fpA"),
    msg("u4", "pB", "USER", 16, "r4", 0, "fpB"),
    msg("m4", "u4", "MODEL", 17, "r4", 1, "fpB"),
], message_selections={"pB": "u4", "p0": "u2"})
plan("group_by_parent_run", [
    run("r1", started=100), run("r2", started=200),
    run("ra", "pr", 100), run("rb", "pr", 200),
], [
    msg("u1", "p0", "USER", 10, "r1", 0, "fpA"),
    msg("m1", "u1", "MODEL", 11, "r1", 1, "fpA"),
    msg("u2", "p0", "USER", 12, "r2", 0, "fpA"),
    msg("m2", "u2", "MODEL", 13, "r2", 1, "fpA"),
    msg("ua", "pa", "USER", 14, "ra", 0, "fpA"),
    msg("ma", "ua", "MODEL", 15, "ra", 1, "fpA"),
    msg("ub", "pa", "USER", 16, "rb", 0, "fpA"),
    msg("mb", "ub", "MODEL", 17, "rb", 1, "fpA"),
])

# ---- fingerprint ------------------------------------------------------------------------------
fingerprint("fingerprint_basic", "hello")
fingerprint("fingerprint_unicode", "Привет, мир", 3)
fingerprint("fingerprint_empty_obj", "text", 0, "{}")
fingerprint("fingerprint_strip_original_uri", "text", 1,
            '{"a":[{"originalUri":"x","b":1}],"originalUri":"y"}')
fingerprint("fingerprint_invalid_json", "text", 2, "not json")
fingerprint("fingerprint_blank_meta", "text", 0, "   ")
fingerprint("fingerprint_numbers", "t", 0,
            '{"n":1.50,"e":1e3,"neg":-0,"bool":true,"nil":null}')
fingerprint("fingerprint_escapes", "t", 0,
            '{"s":"line\\nquote\\"back\\\\slash\\u0001","raw":"héllo"}')
fingerprint("fingerprint_nul_text", "a\u0000b", -1)
fingerprint("fingerprint_scalar_meta", "t", 0, '"top"')
fingerprint("fingerprint_array_meta", "t", 0, '[1,"x",null]')
fingerprint("fingerprint_nested_uri_array", "t", 4,
            '[{"originalUri":"a","keep":[{"originalUri":"b","v":"é"}]}]')


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
