#!/usr/bin/env python3
"""Generates the NativeRunArchivePolicy scenarios (deterministic; rerun after editing)."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def snapshot(name, **overrides):
    case = {
        "name": name,
        "op": "terminalize",
        "id": "r1",
        "conversationId": "conv-a",
        "parentRunId": None,
        "status": "COMPLETED",
        "startedAt": 10,
        "lastCheckpointAt": 20,
        "stopRequestedAt": None,
        "endedAt": None,
        "endReason": None,
        "currentPass": 0,
        "legacyAmbiguous": False,
    }
    case.update(overrides)
    return case


def run(run_id, parent=None, started=1, conversation="conv-a", ambiguous=False):
    return {
        "id": run_id,
        "conversationId": conversation,
        "parentRunId": parent,
        "startedAt": started,
        "legacyAmbiguous": ambiguous,
    }


def ownership(message_id, run_id="r1", sequence=0, conversation="conv-a", consumed=None):
    case = {
        "messageId": message_id,
        "conversationId": conversation,
        "runId": run_id,
        "runSequence": sequence,
    }
    if consumed is not None:
        case["consumedAtPass"] = consumed
    return case


def ownership_case(name, runs, rows, unique=True):
    return {
        "name": name,
        "op": "hasCompleteOwnership",
        "sourceRunIdsWereUnique": unique,
        "runs": runs,
        "ownership": rows,
    }


def order_case(name, runs):
    return {"name": name, "op": "orderByParent", "runs": runs}


CASES = [
    # terminalize: every status, invalid/absent values, recovery and timestamp clamps.
    snapshot("terminalize_active", status="ACTIVE", parentRunId="r-parent", currentPass=2),
    snapshot("terminalize_stopping", status="STOPPING", stopRequestedAt=15),
    snapshot("terminalize_completed", status="COMPLETED"),
    snapshot("terminalize_stopped", status="STOPPED"),
    snapshot("terminalize_failed", status="FAILED"),
    snapshot("terminalize_invalid_status", status="PAUSED"),
    snapshot("terminalize_missing_status", status=""),
    snapshot("terminalize_invalid_end_reason", status="COMPLETED", endReason="DONE"),
    snapshot(
        "terminalize_nonterminal_recovery",
        status="ACTIVE",
        endReason="USER_STOPPED",
        endedAt=99,
        legacyAmbiguous=True,
    ),
    snapshot(
        "terminalize_timestamps_clamp",
        status="STOPPED",
        startedAt=100,
        lastCheckpointAt=50,
        stopRequestedAt=90,
        endedAt=30,
    ),
    snapshot(
        "terminalize_timestamps_preserve",
        status="COMPLETED",
        startedAt=10,
        lastCheckpointAt=20,
        endedAt=99,
    ),
    snapshot("terminalize_pass_clamp", status="COMPLETED", currentPass=-5),
    snapshot(
        "terminalize_invalid_status_valid_reason",
        status="NOPE",
        endReason="USER_STOPPED",
    ),
    # ownership: unique ids, existing run, same conversation, non-negative unique sequences.
    ownership_case(
        "ownership_valid",
        [run("r1"), run("r2")],
        [ownership("m1", "r1", 0, consumed=0), ownership("m2", "r2", 1, consumed=3)],
    ),
    ownership_case("ownership_empty_ownership", [run("r1")], []),
    ownership_case("ownership_empty_runs", [], []),
    ownership_case(
        "ownership_duplicate_message",
        [run("r1"), run("r2")],
        [ownership("m1", "r1", 0), ownership("m1", "r2", 1)],
    ),
    ownership_case(
        "ownership_duplicate_sequence",
        [run("r1")],
        [ownership("m1", "r1", 2), ownership("m2", "r1", 2)],
    ),
    ownership_case("ownership_missing_run", [run("r1")], [ownership("m1", "r9", 0)]),
    ownership_case(
        "ownership_wrong_conversation",
        [run("r1", conversation="conv-a")],
        [ownership("m1", "r1", 0, conversation="conv-b")],
    ),
    ownership_case("ownership_negative_sequence", [run("r1")], [ownership("m1", "r1", -1)]),
    ownership_case(
        "ownership_null_sequence",
        [run("r1")],
        [{"messageId": "m1", "conversationId": "conv-a", "runId": "r1", "runSequence": None}],
    ),
    ownership_case(
        "ownership_nonunique_source",
        [run("r1")],
        [ownership("m1", "r1", 0)],
        unique=False,
    ),
    ownership_case(
        "ownership_duplicate_run_ids_last_wins",
        [run("r1", conversation="conv-a"), run("r1", conversation="conv-b")],
        [ownership("m1", "r1", 0, conversation="conv-b")],
    ),
    # ordering: readiness, missing parents, cycles, duplicate ids and timestamp ties.
    order_case(
        "order_normal_chain",
        [run("c", "b", 30), run("b", "a", 20), run("a", None, 10)],
    ),
    order_case(
        "order_missing_parent",
        [run("orphan", "ghost", 5), run("root", None, 1), run("orphan-child", "orphan", 6)],
    ),
    order_case(
        "order_cycle",
        [run("a", "c", 3), run("b", "a", 1), run("c", "b", 2)],
    ),
    order_case(
        "order_cycle_equal_timestamps",
        [run("a", "c", 0), run("b", "a", 0), run("c", "b", 0)],
    ),
    order_case(
        "order_duplicate_ids",
        [run("r1", None, 3), run("r1", "r2", 1), run("r2", None, 2), run("c", "r1", 4)],
    ),
    order_case(
        "order_duplicate_missing_parent",
        [run("r1", None, 2), run("r1", "ghost", 1)],
    ),
    order_case(
        "order_equal_timestamps",
        [run("b", None, 0), run("a", None, 0), run("c", "a", 0)],
    ),
    order_case("order_empty", []),
]

os.makedirs(OUT, exist_ok=True)
for case in CASES:
    with open(os.path.join(OUT, case["name"] + ".json"), "w", encoding="utf-8") as stream:
        json.dump(case, stream, ensure_ascii=False, indent=2)
        stream.write("\n")
print(f"wrote {len(CASES)} scenarios")
