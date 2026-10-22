#!/usr/bin/env python3
"""Deterministic scenario generator for the runtime recovery stand."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

TERMINAL = ("COMPLETED", "STOPPED", "FAILED")
END_REASONS = {
    "COMPLETED": "MODEL_COMPLETED",
    "STOPPED": "USER_STOPPED",
    "FAILED": "PROVIDER_ERROR",
}


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(identifier: str, branches: str | None = None,
                 data_changed_at: int = 100) -> dict:
    return {"id": identifier, "selectedRunBranchesJson": branches,
            "dataChangedAt": data_changed_at}


def run(identifier: str, conversation_id: str, status: str, parent: str | None = None,
        active_slot: int | None = None, started: int = 1000, stopped: int | None = None,
        ended: int | None = None, end_reason: str | None = None, current_pass: int = 0) -> dict:
    """A RunEntity row that satisfies the entity's init rules."""
    if status in TERMINAL:
        assert active_slot is None, "terminal runs keep a null slot"
        assert ended is not None and end_reason is not None, "terminal runs need ended+reason"
    else:
        assert active_slot == 1, "live runs occupy slot 1"
        assert ended is None and end_reason is None, "live runs have no end"
        if status == "STOPPING":
            assert stopped is not None, "STOPPING records stopRequestedAt"
    entity = {"id": identifier, "conversationId": conversation_id, "status": status,
              "startedAt": started, "lastCheckpointAt": started, "currentPass": current_pass,
              "legacyAmbiguous": False}
    if parent is not None:
        entity["parentRunId"] = parent
    if active_slot is not None:
        entity["activeSlot"] = active_slot
    if stopped is not None:
        entity["stopRequestedAt"] = stopped
    if ended is not None:
        entity["endedAt"] = ended
    if end_reason is not None:
        entity["endReason"] = end_reason
    return entity


def live_run(identifier: str, conversation_id: str, status: str = "ACTIVE",
             **kwargs) -> dict:
    kwargs.setdefault("active_slot", 1)
    if status == "STOPPING":
        kwargs.setdefault("stopped", 1500)
    return run(identifier, conversation_id, status, **kwargs)


def done_run(identifier: str, conversation_id: str, status: str = "COMPLETED",
             **kwargs) -> dict:
    kwargs.setdefault("ended", 2000)
    kwargs.setdefault("end_reason", END_REASONS[status])
    return run(identifier, conversation_id, status, **kwargs)


def message(identifier: str, conversation_id: str, run_id: str, status: str,
            participant: str = "MODEL", text: str = "body", images: list | None = None,
            images_raw: str | None = None, tool_json: str | None = None,
            timestamp: int = 100, run_sequence: int = 0, token_count: int = 0) -> dict:
    entity = {"id": identifier, "conversationId": conversation_id, "text": text,
              "status": status, "participant": participant, "timestamp": timestamp,
              "runId": run_id, "runSequence": run_sequence, "tokenCount": token_count}
    if images is not None:
        entity["images"] = images
    if images_raw is not None:
        entity["imagesRaw"] = images_raw
    if tool_json is not None:
        entity["toolCallJson"] = tool_json
    return entity


def recover(conversation_id: str, at: int) -> dict:
    return {"op": "executeRuntimeRecovery", "conversationId": conversation_id, "at": at}


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-missing-conversation", {
        "steps": [recover("absent", 100)],
    })

    write("02-quiet-conversation", {
        "seed": {
            "conversations": [conversation("c1", '{"r1":"r2"}')],
            "runs": [done_run("r1", "c1"), done_run("r2", "c1", parent="r1")],
            "messages": [
                message("m1", "c1", "r1", status="SUCCESS"),
                message("m2", "c1", "r2", status="STOPPED"),
            ],
        },
        "steps": [
            # Nothing to do: no live run, no stuck statuses, valid branches — so 0 rows change
            # and the watermark must not move. The second step pins the platform failure text
            # for a result-returning statement (java.sql.SQLException: Query returns results).
            recover("c1", 5000),
            {"op": "sql", "sql": "SELECT 1"},
        ],
    })

    write("03-live-run-recovers-messages", {
        "seed": {
            "conversations": [conversation("c1", None)],
            "runs": [
                live_run("r1", "c1"),
                done_run("r0", "c1", status="FAILED", ended=500, end_reason="PROVIDER_ERROR",
                         started=200),
            ],
            "messages": [
                message("m1", "c1", "r1", status="THINKING", timestamp=10, run_sequence=0),
                message("m2", "c1", "r1", status="SUCCESS", timestamp=11, run_sequence=1,
                        tool_json='[{"type":"tool","toolState":"calling","toolName":"shell"}]'),
                message("m3", "c1", "r1", status="SENDING", participant="USER", timestamp=12,
                        run_sequence=2),
                message("m4", "c1", "r1", status="SUCCESS", timestamp=13, run_sequence=3,
                        tool_json='[{"type":"tool","toolState":"succeeded"}]'),
                message("m5", "c1", "r0", status="THINKING", timestamp=5, run_sequence=0),
            ],
        },
        "steps": [recover("c1", 5000)],
    })

    write("04-live-run-stopping", {
        "seed": {
            "conversations": [conversation("c1", None)],
            "runs": [live_run("r1", "c1", status="STOPPING", current_pass=2)],
            "messages": [
                message("m1", "c1", "r1", status="TOOL_CALLING", timestamp=10),
                message("m2", "c1", "r1", status="ERROR", timestamp=11),
            ],
        },
        "steps": [recover("c1", 7000)],
    })

    write("05-branch-repair-branches", {
        "seed": {
            "conversations": [
                conversation("b1", '{"b1r1":"b1r2","b1r2":"b1r2"}'),
                conversation("b2", '{"b2r1":"b2r2"}'),
                conversation("b3", "[1, 2]"),
                conversation("b4", None),
            ],
            "runs": [
                done_run("b1r1", "b1"),
                done_run("b1r2", "b1", parent="b1r1"),
                done_run("b2r1", "b2"),
                done_run("b2r2", "b2", parent="b2r1"),
            ],
        },
        "steps": [
            recover("b1", 5000),
            recover("b2", 5000),
            recover("b3", 5000),
            recover("b4", 5000),
        ],
    })

    write("06-legacy-images-roundtrip", {
        "seed": {
            "conversations": [conversation("c1", None)],
            "runs": [live_run("r1", "c1")],
            "messages": [
                message("m1", "c1", "r1", status="THINKING", timestamp=10, run_sequence=0,
                        images_raw="a.png|||b.png"),
                message("m2", "c1", "r1", status="SUCCESS", timestamp=11, run_sequence=1,
                        images_raw=""),
                message("m3", "c1", "r1", status="THINKING", timestamp=12, run_sequence=2,
                        images=["c.png"]),
            ],
        },
        "steps": [recover("c1", 5000)],
    })

    write("07-tool-json-fallbacks", {
        "seed": {
            "conversations": [conversation("c1", None)],
            "runs": [live_run("r1", "c1")],
            "messages": [
                message("m1", "c1", "r1", status="SUCCESS", timestamp=10, run_sequence=0,
                        tool_json='{"not":"an array"}', images_raw="keep-me"),
                message("m2", "c1", "r1", status="SUCCESS", timestamp=11, run_sequence=1,
                        tool_json='[{"type":"tool","toolState":"succeeded"}]',
                        images_raw="keep-me-too"),
                message("m3", "c1", "r1", status="SUCCESS", timestamp=12, run_sequence=2,
                        tool_json='[null,{"type":"tool","toolName":"shell"}]'),
                message("m4", "c1", "r1", status="SUCCESS", timestamp=13, run_sequence=3,
                        tool_json='[{"type":"tool","toolState":"running","toolName":"shell"}]'),
            ],
        },
        "steps": [recover("c1", 5000)],
    })

    write("08-touch-watermark", {
        "seed": {
            "conversations": [
                conversation("t1", '{"t1r1":"t1r2"}', data_changed_at=9000),
                conversation("t2", '{"t2r1":"t2r2"}', data_changed_at=9000),
            ],
            "runs": [
                done_run("t1r1", "t1"),
                done_run("t1r2", "t1", parent="t1r1"),
                done_run("t2r1", "t2"),
                done_run("t2r2", "t2", parent="t2r1"),
            ],
        },
        "steps": [
            # t1 changes (branch repair) with at below the watermark: MAX(+1, at) keeps it moving
            # forward. t2 changes nothing, so its watermark must stay untouched.
            {"op": "sql", "sql": "UPDATE conversations SET selectedRunBranchesJson = ? " +
             "WHERE id = ?", "args": ['{"t1r1":"t1r2","ghost":"t1r2"}', "t1"]},
            recover("t1", 5000),
            recover("t2", 5000),
        ],
    })

    write("09-all-paths-together", {
        "seed": {
            "conversations": [conversation("c1", '{"r1":"r2","r2":"r2"}')],
            "runs": [live_run("r1", "c1")],
            "messages": [
                message("m1", "c1", "r1", status="THINKING", timestamp=10, run_sequence=0),
                message("m2", "c1", "r1", status="SENDING", participant="USER", timestamp=11,
                        run_sequence=1),
                message("m3", "c1", "r1", status="TOOL_CALLING", timestamp=12, run_sequence=2,
                        tool_json='[{"type":"tool","toolState":"calling"}]',
                        images_raw="x|||y"),
            ],
        },
        "steps": [recover("c1", 5000)],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
