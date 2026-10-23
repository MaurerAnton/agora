#!/usr/bin/env python3
"""Deterministic scenario generator for the context-compact stand."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

END_REASON = {
    "COMPLETED": "MODEL_COMPLETED",
    "STOPPED": "USER_STOPPED",
    "FAILED": "PROVIDER_ERROR",
}
TERMINAL = ("COMPLETED", "STOPPED", "FAILED")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(identifier: str, branches: str | None = None,
                 run_branches: str | None = None, last_updated: int = 100,
                 data_changed_at: int = 100) -> dict:
    return {"id": identifier, "selectedBranchesJson": branches,
            "selectedRunBranchesJson": run_branches, "lastUpdated": last_updated,
            "dataChangedAt": data_changed_at}


def run(identifier: str, conversation_id: str, status: str = "COMPLETED",
        parent: str | None = None, started: int = 1000, stopped: int | None = None,
        ended: int | None = None, reason: str | None = None, current_pass: int = 0) -> dict:
    """A RunEntity row that satisfies the entity's init rules."""
    if status in TERMINAL:
        active_slot = None
        ended = started + 100 if ended is None else ended
        reason = reason or END_REASON[status]
        stopped = None
    else:
        active_slot = 1
        ended = None
        reason = None
        if status == "STOPPING" and stopped is None:
            stopped = started + 50
    entity = {"id": identifier, "conversationId": conversation_id, "status": status,
              "activeSlot": active_slot, "startedAt": started, "lastCheckpointAt": started,
              "stopRequestedAt": stopped, "endedAt": ended, "endReason": reason,
              "currentPass": current_pass, "legacyAmbiguous": False}
    if parent is not None:
        entity["parentRunId"] = parent
    return entity


def message(identifier: str, conversation_id: str, text: str = "body", status: str = "SUCCESS",
            participant: str = "MODEL", parent: str | None = None, images: list | None = None,
            images_raw: str | None = None, timestamp: int = 100, run_id: str = "r1",
            seq: int = 0, token_count: int = 0, model_name: str | None = None) -> dict:
    entity = {"id": identifier, "conversationId": conversation_id, "text": text,
              "status": status, "participant": participant, "timestamp": timestamp,
              "runId": run_id, "runSequence": seq, "tokenCount": token_count}
    if parent is not None:
        entity["parentId"] = parent
    if images is not None:
        entity["images"] = images
    if images_raw is not None:
        entity["imagesRaw"] = images_raw
    if model_name is not None:
        entity["modelName"] = model_name
    return entity


def embedding(identifier: int, message_id: str) -> dict:
    return {"id": identifier, "messageId": message_id, "modelId": "m", "embedding": "0a0b",
            "chunkText": "chunk", "dimension": 2, "sourceFingerprint": "fp"}


def replacement(identifier: str, conversation_id: str, parent: str | None = None,
                status: str = "ACTIVE", started: int = 50) -> dict:
    """The replacement RunEntity argument of beginRecompactContextCompact."""
    entity = run(identifier, conversation_id, status, parent=parent, started=started)
    if status == "ACTIVE":
        entity["activeSlot"] = 1
        entity["endedAt"] = None
        entity["endReason"] = None
        entity["stopRequestedAt"] = None
    return entity


def recompact(replacement_run: dict, message_id: str, model_name: str,
              expected_branches: str) -> dict:
    return {"op": "beginRecompactContextCompact", "replacementRun": replacement_run,
            "messageId": message_id, "modelName": model_name,
            "expectedSelectedBranchesJson": expected_branches}


def remove(message_id: str) -> dict:
    return {"op": "removeContextCompact", "messageId": message_id}


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # 01 — the full Recompact surgery: run swap, message re-point, selections rewrite, old run
    # dropped, watermark moved to the replacement run's startedAt.
    write("01-recompact-happy", {
        "seed": {
            "conversations": [conversation(
                "c1", '{"m1":"compact_1"}', '{"r1":"compact_run"}')],
            "runs": [
                run("r1", "c1", started=10, ended=20, reason="MODEL_COMPLETED"),
                run("compact_run", "c1", "STOPPED", parent="r1", started=30, ended=40,
                    reason="USER_STOPPED"),
            ],
            "messages": [
                message("m1", "c1", text="hello", status="SUCCESS", participant="USER",
                        timestamp=5, run_id="r1", seq=0),
                message("compact_1", "c1", text="summary", status="SUCCESS", parent="m1",
                        timestamp=45, run_id="compact_run", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("recompact_run", "c1", parent="r1"), "compact_1", "gpt-x",
                      '{"m1":"compact_1"}'),
        ],
    })

    # 02 — Remove with dedicated run and suffix children: both promotion branches fire, both
    # reparents run, the embeddings go, and the clock watermark is masked.
    write("02-remove-dedicated-run", {
        "seed": {
            "conversations": [conversation(
                "c1", '{"m1":"compact_1","compact_1":"m2"}',
                '{"r1":"compact_run","compact_run":"child_run"}')],
            "runs": [
                run("r1", "c1"),
                run("compact_run", "c1", "STOPPED", parent="r1"),
                run("child_run", "c1", parent="compact_run"),
            ],
            "messages": [
                message("m1", "c1", status="SUCCESS", participant="USER", run_id="r1", seq=0),
                message("compact_1", "c1", status="SUCCESS", parent="m1", run_id="compact_run",
                        seq=0),
                message("m2", "c1", status="SUCCESS", parent="compact_1", run_id="r1", seq=1),
            ],
            "embeddings": [embedding(1, "compact_1"), embedding(2, "m1")],
        },
        "steps": [remove("compact_1")],
    })

    # 03 — Remove where neither projection has a suffix entry: the parent keys are dropped
    # instead of being repointed.
    write("03-remove-key-removal", {
        "seed": {
            "conversations": [conversation(
                "c1", '{"m1":"compact_1"}', '{"r1":"compact_run"}')],
            "runs": [
                run("r1", "c1"),
                run("compact_run", "c1", "STOPPED", parent="r1"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_1", "c1", parent="m1", run_id="compact_run", seq=0),
            ],
        },
        "steps": [remove("compact_1")],
    })

    # 04 — The compact shares its Run with another message, so the Run survives untouched while
    # the message-level projection still collapses.
    write("04-remove-shared-run", {
        "seed": {
            "conversations": [conversation(
                "c1", '{"m1":"compact_1"}', '{"r1":"compact_run"}')],
            "runs": [
                run("r1", "c1"),
                run("compact_run", "c1", "STOPPED", parent="r1"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_1", "c1", parent="m1", run_id="compact_run", seq=0),
                message("other_msg", "c1", status="SUCCESS", run_id="compact_run", seq=1),
            ],
        },
        "steps": [remove("compact_1")],
    })

    # 05 — The guards SQLite's foreign keys would otherwise make unreachable: reached by turning
    # foreign_keys off for the deletions, exactly as the two `?: return false` paths and the
    # missing-conversation error behave.
    write("05-fk-off-guards", {
        "seed": {
            "conversations": [
                conversation("c1", '{"m1":"compact_c1"}'),
                conversation("c2"),
                conversation("c3"),
            ],
            "runs": [
                run("c1_run", "c1"),
                run("c2_run", "c2"),
                run("c3_run", "c3"),
            ],
            "messages": [
                message("m1", "c1", run_id="c1_run", seq=0),
                message("compact_c1", "c1", parent="m1", run_id="c1_run", seq=1),
                message("compact_c2", "c2", run_id="c2_run", seq=0),
                message("compact_c3", "c3", run_id="c3_run", seq=0),
            ],
        },
        "steps": [
            remove("missing_message"),
            {"op": "sql", "sql": "PRAGMA foreign_keys = OFF"},
            {"op": "sql", "sql": "DELETE FROM conversations WHERE id = ?", "args": ["c1"]},
            remove("compact_c1"),
            {"op": "sql", "sql": "DELETE FROM runs WHERE id = ?", "args": ["c2_run"]},
            remove("compact_c2"),
            {"op": "sql", "sql": "DELETE FROM conversations WHERE id = ?", "args": ["c3"]},
            recompact(replacement("zz_run", "c3", parent="c3_run"), "compact_c3", "m",
                      '{"m1":"compact_c1"}'),
        ],
    })

    # 06 — A non-compact message is rejected by the prefix require.
    write("06-remove-not-compact", {
        "seed": {
            "conversations": [conversation("c1")],
            "runs": [run("r1", "c1")],
            "messages": [message("m1", "c1", run_id="r1", seq=0)],
        },
        "steps": [remove("m1")],
    })

    # 07 — The read half of the interface, including every "absent row" answer.
    write("07-plain-reads", {
        "seed": {
            "conversations": [conversation("c1", '{"a":"b"}', '{"c":"d"}'),
                              conversation("c2")],
            "runs": [
                run("r1", "c1"),
                run("r2", "c1"),
                run("lr", "c2", "ACTIVE", started=10),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0, timestamp=5),
                message("m2", "c1", run_id="r1", seq=5, timestamp=9),
            ],
        },
        "steps": [
            {"op": "getConversation", "conversationId": "c1"},
            {"op": "getConversation", "conversationId": "missing"},
            {"op": "getMessage", "messageId": "m1"},
            {"op": "getMessage", "messageId": "missing"},
            {"op": "getRun", "runId": "r1"},
            {"op": "getRun", "runId": "missing"},
            {"op": "getLiveRun", "conversationId": "c1"},
            {"op": "getLiveRun", "conversationId": "c2"},
            {"op": "getLiveRun", "conversationId": "missing"},
            {"op": "getMessagesForRuns", "runIds": ["r1"]},
            {"op": "getMessagesForRuns", "runIds": ["missing"]},
            {"op": "nextRunSequence", "runId": "r1"},
            {"op": "nextRunSequence", "runId": "r2"},
        ],
    })

    # 08 — The write half of the interface: every update/delete/insert reports its row count,
    # the four selection updaters move the two watermarks independently, and the Compact run
    # replacement both succeeds (1) and refuses (0).
    write("08-plain-writes", {
        "seed": {
            "conversations": [conversation("c1")],
            "runs": [
                run("r1", "c1"),
                run("r_child", "c1", parent="r1"),
                run("r_del", "c1"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("m2", "c1", parent="m1", run_id="r1", seq=1),
                message("m3", "c1", run_id="r1", seq=2),
                message("compact_1", "c1", status="SUCCESS", run_id="r1", seq=3),
            ],
            "embeddings": [embedding(1, "m1"), embedding(2, "m2")],
        },
        "steps": [
            {"op": "insertMessage",
             "message": {"id": "m_new", "conversationId": "c1", "text": "inserted",
                         "status": "SUCCESS", "participant": "MODEL", "timestamp": 50,
                         "runId": "r1", "runSequence": 7}},
            {"op": "insertRun",
             "run": run("r_new", "c1", started=70)},
            {"op": "touchRun", "runId": "r1", "at": 77},
            {"op": "touchRun", "runId": "missing", "at": 77},
            {"op": "reparentMessageChildren", "removedMessageId": "m1",
             "replacementParentId": "m3"},
            {"op": "reparentMessageChildren", "removedMessageId": "m1",
             "replacementParentId": None},
            {"op": "reparentRunChildren", "removedRunId": "r1",
             "replacementParentRunId": "r_new"},
            {"op": "reparentRunChildren", "removedRunId": "missing",
             "replacementParentRunId": None},
            {"op": "replaceContextCompactMessageRun", "messageId": "compact_1",
             "oldRunId": "r1", "newRunId": "r_new", "modelName": "model-x"},
            {"op": "replaceContextCompactMessageRun", "messageId": "compact_1",
             "oldRunId": "r1", "newRunId": "r_new", "modelName": "model-x"},
            {"op": "updateSelectionsForRunDeletion", "conversationId": "c1",
             "selectedBranchesJson": '{"a":"b"}', "selectedRunBranchesJson": '{"c":"d"}',
             "at": 500},
            {"op": "updateMessageBranchSelections", "conversationId": "c1",
             "selectedBranchesJson": '{"e":"f"}', "at": 600},
            {"op": "updateRunBranchSelections", "conversationId": "c1",
             "selectedRunBranchesJson": '{"g":"h"}', "at": 700},
            {"op": "updateBranchSelections", "conversationId": "c1",
             "selectedBranchesJson": '{"i":"j"}', "selectedRunBranchesJson": '{"k":"l"}',
             "at": 800},
            {"op": "updateMessageBranchSelections", "conversationId": "missing",
             "selectedBranchesJson": "{}", "at": 900},
            {"op": "deleteMessagesByIds", "ids": ["m3"]},
            {"op": "deleteEmbeddingsByMessageIds", "messageIds": ["m1"]},
            {"op": "deleteRun", "runId": "r_del"},
            {"op": "deleteRun", "runId": "missing"},
        ],
    })

    # 09 — RunEntity validation on the read path: the init block and Enum.valueOf both fire.
    # One corrupt Run per conversation: SQLite's unique (conversationId, activeSlot) index only
    # permits a single occupied slot, so the violations cannot share a conversation.
    write("09-run-validation", {
        "seed": {
            "conversations": [
                conversation("c_slot"), conversation("c_enum"), conversation("c_end"),
                conversation("c_stop"), conversation("c_pass"),
            ],
            "runs": [
                {"id": "bad_slot", "conversationId": "c_slot", "status": "COMPLETED",
                 "activeSlot": 1, "startedAt": 10, "lastCheckpointAt": 10, "endedAt": 20,
                 "endReason": "MODEL_COMPLETED", "currentPass": 0, "legacyAmbiguous": False},
                {"id": "bad_enum", "conversationId": "c_enum", "status": "BOGUS",
                 "activeSlot": None, "startedAt": 10, "lastCheckpointAt": 10, "endedAt": None,
                 "endReason": None, "currentPass": 0, "legacyAmbiguous": False},
                {"id": "bad_end", "conversationId": "c_end", "status": "COMPLETED",
                 "activeSlot": None, "startedAt": 10, "lastCheckpointAt": 10, "endedAt": None,
                 "endReason": None, "currentPass": 0, "legacyAmbiguous": False},
                {"id": "bad_stop", "conversationId": "c_stop", "status": "STOPPING",
                 "activeSlot": 1, "startedAt": 10, "lastCheckpointAt": 10,
                 "stopRequestedAt": None, "endedAt": None, "endReason": None, "currentPass": 0,
                 "legacyAmbiguous": False},
                {"id": "bad_pass", "conversationId": "c_pass", "status": "ACTIVE",
                 "activeSlot": 1, "startedAt": 10, "lastCheckpointAt": 10, "endedAt": None,
                 "endReason": None, "currentPass": -1, "legacyAmbiguous": False},
            ],
        },
        "steps": [
            {"op": "getRun", "runId": "bad_slot"},
            {"op": "getRun", "runId": "bad_enum"},
            {"op": "getRun", "runId": "bad_end"},
            {"op": "getRun", "runId": "bad_stop"},
            {"op": "getRun", "runId": "bad_pass"},
            {"op": "getLiveRun", "conversationId": "c_slot"},
        ],
    })

    # 10 — Recompact guards that fire before any read of the compact Run.
    write("10-recompact-errors-early", {
        "seed": {
            "conversations": [conversation("c1"), conversation("cb")],
            "runs": [
                run("r1", "c1"),
                run("busy_run", "cb", "ACTIVE", started=10),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_x", "cb", run_id="busy_run", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("zz1", "c1"), "missing_message", "m", "{}"),
            recompact(replacement("zz2", "c1"), "m1", "m", "{}"),
            recompact(replacement("zz3", "cb"), "compact_x", "m", "{}"),
        ],
    })

    # 11 — Recompact guards over the compact Run and the compact message themselves.
    write("11-recompact-errors-run", {
        "seed": {
            "conversations": [conversation("c1"), conversation("c2")],
            "runs": [
                run("r1", "c1"),
                run("compact_shared", "c1", "STOPPED", parent="r1"),
                run("compact_ok", "c1", "STOPPED", parent="r1"),
                run("active_c2", "c2", "ACTIVE", started=10),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_1", "c1", parent="m1", run_id="compact_shared", seq=0),
                message("compact_2", "c1", parent="m1", run_id="compact_shared", seq=1),
                message("compact_3", "c1", parent="m1", run_id="active_c2", seq=0),
                message("compact_4", "c1", parent="m1", status="THINKING",
                        run_id="compact_ok", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("zz_shared", "c1", parent="r1"), "compact_1", "m",
                      "{}"),
            recompact(replacement("zz_active", "c2", parent="r1"), "compact_3", "m",
                      "{}"),
            recompact(replacement("zz_thinking", "c1", parent="r1"), "compact_4", "m",
                      "{}"),
        ],
    })

    # 12 — The branch projection check: content mismatch fails, and two maps that differ only in
    # key order are equal (Kotlin Map.equals is order independent) so the Recompact succeeds.
    write("12-recompact-branch-check", {
        "seed": {
            "conversations": [
                conversation("c1", '{"x":"y"}'),
                conversation("c2", '{"b":"x","a":"y"}'),
            ],
            "runs": [
                run("r1", "c1"),
                run("compact_c1", "c1", "STOPPED", parent="r1"),
                run("r2", "c2"),
                run("compact_c2", "c2", "STOPPED", parent="r2"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_c1", "c1", parent="m1", run_id="compact_c1", seq=0),
                message("m2", "c2", run_id="r2", seq=0),
                message("compact_c2", "c2", parent="m2", run_id="compact_c2", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("zz_a", "c1", parent="r1"), "compact_c1", "m",
                      '{"x":"z"}'),
            recompact(replacement("zz_b", "c2", parent="r2"), "compact_c2", "m",
                      '{"a":"y","b":"x"}'),
        ],
    })

    # 13 — The remaining Recompact guards: parent, fresh Run, conversation, status/slot,
    # parentage and the run-graph consistency check.
    write("13-recompact-errors-late", {
        "seed": {
            "conversations": [
                conversation("c1", '{"m1":"compact_ok"}',
                             '{"r1":"compact_graph","other":"compact_graph"}'),
                conversation("c2"),
            ],
            "runs": [
                run("r1", "c1"),
                run("other_run", "c1"),
                run("compact_parentless", "c1", "STOPPED", parent="r1"),
                run("compact_ok", "c1", "STOPPED", parent="r1"),
                run("compact_ancestry", "c1", "STOPPED", parent="other_run"),
                run("compact_graph", "c1", "STOPPED", parent="r1"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_parentless", "c1", status="SUCCESS", run_id="compact_parentless",
                        seq=0),
                message("compact_ok", "c1", parent="m1", run_id="compact_ok", seq=0),
                message("compact_ancestry", "c1", parent="m1", run_id="compact_ancestry",
                        seq=0),
                message("compact_graph", "c1", parent="m1", run_id="compact_graph", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("zz_c", "c1", parent="r1"), "compact_parentless", "m",
                      '{"m1":"compact_ok"}'),
            recompact(replacement("compact_ok", "c1", parent="r1"), "compact_ok", "m",
                      '{"m1":"compact_ok"}'),
            recompact(replacement("zz_wrong", "c2", parent="r1"), "compact_ok", "m",
                      '{"m1":"compact_ok"}'),
            recompact(replacement("zz_terminal", "c1", parent="r1", status="COMPLETED"),
                      "compact_ok", "m", '{"m1":"compact_ok"}'),
            recompact(replacement("zz_parent", "c1", parent="other_run"), "compact_ok", "m",
                      '{"m1":"compact_ok"}'),
            recompact(replacement("zz_ancestry", "c1", parent="r1"), "compact_ancestry", "m",
                      '{"m1":"compact_ok"}'),
            recompact(replacement("zz_graph", "c1", parent="r1"), "compact_graph", "m",
                      '{"m1":"compact_ok"}'),
        ],
    })

    # 14 — A null branch projection decodes to an empty map (expected "{}" matches) and the
    # write-back materialises "{}" over the NULL column.
    write("14-recompact-null-branches", {
        "seed": {
            "conversations": [conversation("c1", None, None)],
            "runs": [
                run("r1", "c1"),
                run("compact_ok", "c1", "STOPPED", parent="r1"),
            ],
            "messages": [
                message("m1", "c1", run_id="r1", seq=0),
                message("compact_ok", "c1", parent="m1", run_id="compact_ok", seq=0),
            ],
        },
        "steps": [
            recompact(replacement("zz_null", "c1", parent="r1"), "compact_ok", "m", "{}"),
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
