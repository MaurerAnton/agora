#!/usr/bin/env python3
"""Deterministic scenario generator for NativeConversationGraphImporter.

Each scenario carries: the v5 (or legacy v4) graph entries the importer streams, the seeded
database rows (MERGE id sets, existing runs/messages to skip or keep, embeddings to clear),
the injected resolveSystemPromptId map and RestoredMedia, and the op sequence
(count / headers / import). Seed SQL uses the full-column INSERT style of the other data-layer
stands so NULL semantics are explicit.
"""
from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

INDEX = "conv/index.json"
TASKS = "conv/tasks.json"


def conversation_entry(conversation_id: str) -> str:
    return "conv/items/" + conversation_id.encode("utf-8").hex() + ".json"


def index(*conversations) -> str:
    items = []
    for item in conversations:
        entry = dict(item)
        entry.setdefault("dataChangedAt", 1)
        items.append(entry)
    return json.dumps(
        {"conversations": items},
        separators=(",", ":"),
        ensure_ascii=False,
    )


def graph_item(conversation_id: str, payload: dict) -> str:
    """A conv/items/<hex>.json payload: the importer's field arrays."""
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def sql_value(value):
    if value is None:
        return "NULL"
    if isinstance(value, bool):
        return "1" if value else "0"
    if isinstance(value, (int, float)):
        return str(value)
    return "'" + str(value).replace("'", "''") + "'"


def insert(table, values):
    columns = ", ".join(values)
    return (
        f"INSERT INTO {table} ({columns}) VALUES ("
        + ", ".join(sql_value(value) for value in values.values())
        + ")"
    )


def seed_conversation(conv_id, title="t", **extra):
    values = {
        "id": conv_id,
        "title": title,
        "lastUpdated": 1,
        "dataChangedAt": 1,
        "origin": "user",
        "graduated": 0,
        "draftText": "",
        "hasUnreadGeneration": 0,
        "isPinned": 0,
    }
    values.update(extra)
    return insert("conversations", values)


def seed_run(run_id, conv_id, **extra):
    values = {
        "id": run_id,
        "conversationId": conv_id,
        "parentRunId": None,
        "status": "COMPLETED",
        "activeSlot": None,
        "startedAt": 1,
        "lastCheckpointAt": 1,
        "stopRequestedAt": None,
        "endedAt": 2,
        "endReason": "MODEL_COMPLETED",
        "currentPass": 0,
        "legacyAmbiguous": 0,
    }
    values.update(extra)
    return insert("runs", values)


def seed_message(msg_id, conv_id, run_id=None, **extra):
    values = {
        "id": msg_id,
        "conversationId": conv_id,
        "parentId": None,
        "text": "seed",
        "images": "[]",
        "tokenCount": 0,
        "status": "SUCCESS",
        "participant": "MODEL",
        "timestamp": 1,
        "runId": run_id,
        "runSequence": -1,
        "consumedAtPass": None,
        "sourceJson": None,
        "toolCallJson": None,
        "attachmentMeta": None,
        "modelName": None,
    }
    values.update(extra)
    return insert("messages", values)


def seed_task(task_id, **extra):
    values = {
        "id": task_id,
        "name": "n",
        "prompt": "p",
        "systemPrompt": None,
        "systemPromptId": None,
        "modelId": None,
        "cronExpr": "0 9 * * *",
        "runAt": None,
        "nextRunAt": 7,
        "enabled": 1,
        "createdAt": 1,
        "lastRunAt": None,
    }
    values.update(extra)
    return insert("tasks", values)


def seed_loop(conv_id, **extra):
    values = {
        "conversationId": conv_id,
        "intervalMs": 60000,
        "prompt": None,
        "nextFireAt": 99,
        "cycleCount": 0,
        "maxCycles": None,
        "active": 1,
        "revision": 3,
    }
    values.update(extra)
    return insert("loops", values)


def seed_embedding(row_id, message_id, model_id="m-x"):
    return (
        "INSERT INTO embeddings (id, messageId, modelId, embedding, chunkText, dimension, "
        "sourceFingerprint) VALUES ("
        + ", ".join([
            sql_value(row_id), sql_value(message_id), sql_value(model_id),
            "x'00'", "'chunk'", "2", "'fp'",
        ])
        + ")"
    )


def write(name: str, **scenario):
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def write_scenario(name: str, *, steps, **rest):
    scenario = {
        "appVersion": "9.9.9",
        "strategy": rest.pop("strategy", "MERGE"),
        "graphVersion": rest.pop("graphVersion", 5),
        "semanticSnapshot": rest.pop("semanticSnapshot", [None, []]),
        "steps": steps,
    }
    scenario.update(rest)
    write(name, **scenario)


def gconv(conv_id, title="t", **extra):
    value = {"id": conv_id, "title": title, "lastUpdated": 1}
    value.update(extra)
    return value


def grun(run_id, conv_id, started=1, **extra):
    value = {
        "id": run_id,
        "conversationId": conv_id,
        "status": "COMPLETED",
        "startedAt": started,
        "lastCheckpointAt": started,
        "endedAt": started + 1,
        "endReason": "MODEL_COMPLETED",
    }
    value.update(extra)
    return value


def gmsg(msg_id, conv_id, ts, participant="MODEL", status="SUCCESS", run_id=None,
         run_seq=None, parent=None, text="t", images=None, attachment_meta=None,
         **extra):
    value = {
        "id": msg_id,
        "conversationId": conv_id,
        "text": text,
        "timestamp": ts,
        "participant": participant,
        "status": status,
    }
    if run_id is not None:
        value["runId"] = run_id
    if run_seq is not None:
        value["runSequence"] = run_seq
    if parent is not None:
        value["parentId"] = parent
    if images is not None:
        value["images"] = images
    if attachment_meta is not None:
        value["attachmentMeta"] = attachment_meta
    value.update(extra)
    return value


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".json"):
            os.remove(os.path.join(OUT, name))

    c1 = conversation_entry("c1")
    c2 = conversation_entry("c2")

    # 01 — counts over the chained v5 graph (conversation bodies included).
    write_scenario(
        "01-count",
        entries={
            INDEX: index(
                {"id": "c1", "entry": c1},
                {"id": "c2", "entry": c2},
            ),
            c1: graph_item("c1", {
                "conversations": [gconv("c1")],
                "runs": [],
                "messages": [],
                "loops": [{"conversationId": "c1", "intervalMs": 60000}],
            }),
            c2: graph_item("c2", {
                "conversations": [gconv("c2")],
                "runs": [],
                "messages": [],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": [{"id": "t1", "name": "n", "prompt": "p",
                                          "cronExpr": "0 9 * * *", "createdAt": 1}]},
                              separators=(",", ":")),
        },
        steps=[{"op": "count"}],
    )

    # 02 — headers under REPLACE: dangling task cleared, unread dropped, prompt/task id
    # resolution, run parent ordering, loop sanitize, draft restore, settings extraction.
    write_scenario(
        "02-headers-replace",
        strategy="REPLACE",
        resolveSystemPromptIds={"old-p": "resolved-p"},
        entries={
            INDEX: index(
                {"id": "c1", "entry": c1},
                {"id": "c2", "entry": c2},
            ),
            c1: graph_item("c1", {
                "conversations": [gconv(
                    "c1",
                    taskId="task-missing",
                    systemPromptId="old-p",
                    hasUnreadGeneration=True,
                    draftAttachments=json.dumps(
                        [{"localId": "a1", "uri": "media/images/e.png", "type": "image"}],
                        separators=(",", ":"),
                    ),
                    conversationSettings={"temperature": 0.7, "maxTokens": 512},
                )],
                # raw order is child-before-parent: headers must order parents first
                "runs": [
                    grun("r2", "c1", started=20, parentRunId="r1"),
                    grun("r1", "c1", started=10, parentRunId=None),
                ],
                "messages": [],
                "loops": [{"conversationId": "c1", "intervalMs": 60000,
                           "prompt": "  keep  ", "cycleCount": -4, "active": True}],
            }),
            c2: graph_item("c2", {
                "conversations": [gconv("c2")],
                "runs": [],
                "messages": [],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": [{"id": "t1", "name": "  named  ",
                                          "prompt": "  body  ", "systemPromptId": "old-p",
                                          "cronExpr": " 0 9 * * * ", "nextRunAt": 9,
                                          "enabled": True, "createdAt": 1}]},
                              separators=(",", ":")),
        },
        restoredMedia={
            "archiveFiles": {
                "media/images/e.png": {"absolutePath": "/restored/e.png",
                                       "uri": "content://restored/e.png"},
            },
        },
        steps=[{"op": "headers"}],
    )

    # 03 — headers under MERGE: existing id sets keep the dangling task, duplicate run ids
    # make the source unique flag false, and the id arrays carry the query+graph insertion order.
    write_scenario(
        "03-headers-merge",
        strategy="MERGE",
        seedSql=[
            seed_conversation("keep", title="existing"),
            seed_task("existing-task"),
        ],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv("c1", taskId="existing-task")],
                "runs": [grun("dup", "c1", started=1),
                         grun("dup", "c1", started=2)],
                "messages": [],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "headers"}],
    )

    # 04 — import under MERGE: upserts overwrite, the existing run is NOT re-inserted,
    # existing messages survive unless they carry images, non-available conversations skip,
    # task/loop sanitize, image restore, embedding clear, transfer + maintenance + ledger.
    write_scenario(
        "04-import-merge",
        strategy="MERGE",
        resolveSystemPromptIds={"old": "new"},
        semanticSnapshot=["emb-model", ["c1"]],
        seedSql=[
            seed_conversation("c1", title="OLD"),
            seed_run("r1", "c1", lastCheckpointAt=42, endedAt=77),
            seed_message("m1", "c1", text="old m1", runId="r1", runSequence=0),
            seed_task("t-keep", enabled=1, nextRunAt=7),
            seed_embedding(1, "m1"),
        ],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv(
                    "c1", title="NEW", taskId="t-keep", systemPromptId="old",
                )],
                "runs": [grun("r1", "c1", started=99, endedAt=100)],
                "messages": [
                    gmsg("m1", "c1", 10, run_id="r1", run_seq=0, text="new m1",
                         images=["media/images/x.png"]),
                    gmsg("m2", "c1", 11, run_id="r1", run_seq=1, parent="m1"),
                    gmsg("m3", "other", 12, run_id="r1", run_seq=0),
                ],
                "loops": [{"conversationId": "c1", "intervalMs": 60000,
                           "prompt": "  loop  ", "cycleCount": -1, "active": True}],
            }),
            TASKS: json.dumps({"tasks": [
                {"id": "t-keep", "name": "  named  ", "prompt": "  CHANGED  ",
                 "cronExpr": " 0 9 * * * ", "nextRunAt": 9, "enabled": True,
                 "createdAt": 1},
                {"id": "t-new", "name": "n2", "prompt": "p2",
                 "cronExpr": "*/5 * * * *", "createdAt": 2},
            ]}, separators=(",", ":")),
        },
        restoredMedia={
            "archiveFiles": {
                "media/images/x.png": {"absolutePath": "/restored/x.png",
                                       "uri": "content://restored/x.png"},
            },
        },
        steps=[{"op": "import"}],
    )

    # 05 — import with incomplete ownership: the legacy backfill planner derives the runs,
    # the assignments and the run-branch selections (re-encoded into the conversation row).
    write_scenario(
        "05-import-backfill",
        strategy="MERGE",
        seedSql=[seed_conversation("c1", selectedBranchesJson='{"u1":"m1"}')],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv(
                    "c1",
                    selectedBranchesJson='{"u1":"m1"}',
                    selectedRunBranchesJson='{"r1":"r1"}',
                )],
                "runs": [],
                "messages": [
                    gmsg("u1", "c1", 10, participant="USER", parent="p0", text="again"),
                    gmsg("m1", "c1", 11, parent="u1"),
                ],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "import"}],
    )

    # 06 — import under REPLACE: everything seeded is wiped (cascades included), the
    # embeddings are cleared wholesale, the transfer records MODE_REPLACE, and the maintenance
    # + semantic mutation rows land with the masked clock.
    write_scenario(
        "06-import-replace",
        strategy="REPLACE",
        semanticSnapshot=["emb-model", ["c1"]],
        seedSql=[
            seed_conversation("gone", title="GONE"),
            seed_run("gr", "gone"),
            seed_task("gone-task"),
            seed_loop("gone"),
            seed_message("gm", "gone", run_id="gr"),
            seed_embedding(1, "any"),
        ],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv("c1")],
                "runs": [grun("r1", "c1")],
                "messages": [gmsg("m1", "c1", 10, run_id="r1", run_seq=0)],
                "loops": [{"conversationId": "c1", "intervalMs": 60000}],
            }),
            TASKS: json.dumps({"tasks": [{"id": "t1", "name": "n", "prompt": "p",
                                          "cronExpr": "0 9 * * *", "createdAt": 1}]},
                              separators=(",", ":")),
        },
        steps=[{"op": "import"}],
    )

    # 07 — import with complete ownership where the repair planner infers a run parent:
    # r2 becomes r1's child (legacyAmbiguous), the leading duplicate input u2 is deleted,
    # m2 is reparented and resequenced, a PROCESS_RECOVERED run stops its SENDING model row,
    # and the conversation's branch selections are re-encoded.
    write_scenario(
        "07-import-repair",
        strategy="MERGE",
        semanticSnapshot=[None, []],
        seedSql=[
            seed_conversation(
                "c1",
                selectedBranchesJson='{"u1":"m1"}',
                selectedRunBranchesJson='{"r1":"r1"}',
            ),
        ],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv(
                    "c1",
                    selectedBranchesJson='{"u1":"m1"}',
                    selectedRunBranchesJson='{"r1":"r1"}',
                )],
                "runs": [
                    grun("r1", "c1", started=10),
                    grun("r2", "c1", started=20),
                    grun("r3", "c1", started=30, endReason="PROCESS_RECOVERED"),
                ],
                "messages": [
                    gmsg("u1", "c1", 5, participant="USER", parent="p0",
                         run_id="r1", run_seq=0, text="same input"),
                    gmsg("m1", "c1", 11, parent="u1", run_id="r1", run_seq=1),
                    gmsg("u2", "c1", 12, participant="USER", parent="p0",
                         run_id="r2", run_seq=0, text="same input"),
                    gmsg("m2", "c1", 13, parent="u2", run_id="r2", run_seq=1),
                    gmsg("m3", "c1", 31, status="SENDING", run_id="r3", run_seq=0),
                ],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "import"}],
    )

    # 08 — the legacy (version < 5) stream: counts and headers over conversations.json.
    write_scenario(
        "08-legacy-headers",
        graphVersion=4,
        entries={
            "conversations.json": json.dumps({
                "conversations": [gconv("c1", taskId="task-x")],
                "runs": [grun("r1", "c1")],
                "messages": [],
                "loops": [],
                "tasks": [{"id": "task-x", "name": "n", "prompt": "p",
                           "cronExpr": "0 9 * * *", "createdAt": 1}],
            }, separators=(",", ":")),
        },
        steps=[{"op": "count"}, {"op": "headers"}],
    )

    # 09 — a conversation without the required id: kotlinx decode failure inside the headers read.
    write_scenario(
        "09-headers-missing-id",
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [{"title": "no id", "lastUpdated": 1}],
                "runs": [],
                "messages": [],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "headers"}],
    )

    # 10 — archiveVersion < 4: message images and attachment metadata restore through the
    # legacy images/videos-by-message maps instead of the archiveFiles index.
    write_scenario(
        "10-import-legacy-media",
        strategy="MERGE",
        semanticSnapshot=[None, []],
        seedSql=[seed_conversation("c1")],
        entries={
            INDEX: index({"id": "c1", "entry": c1}),
            c1: graph_item("c1", {
                "conversations": [gconv("c1")],
                "runs": [grun("r1", "c1")],
                "messages": [
                    gmsg("m1", "c1", 10, run_id="r1", run_seq=0, images=[],
                         attachment_meta=json.dumps({"items": [
                             {"originalUri": "archive/video0", "type": "video",
                              "file_size": 9},
                             {"type": "image", "image_index": 0, "page_count": 1,
                              "originalUri": "archive/image0"},
                         ]}, separators=(",", ":"))),
                ],
                "loops": [],
            }),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        restoredMedia={
            "archiveFiles": {},
            "legacyImagesByMessage": {"m1": {"0": "legacy://images/m1/0"}},
            "legacyVideosByMessage": {"m1": {"0": "legacy://videos/m1/0"}},
        },
        steps=[{"op": "import", "archiveVersion": 3}],
    )

    print(f"Wrote {len([n for n in os.listdir(OUT) if n.endswith('.json')])} scenarios to {OUT}")


if __name__ == "__main__":
    main()
