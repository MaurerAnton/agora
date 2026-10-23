#!/usr/bin/env python3
"""Writes the ConversationExportSnapshotReader scenarios.

The reader is a read-only traversal, so the scenarios seed raw schema-36 rows and vary only the
reader's two callbacks: includeBody (per conversation) and onRecord (the emitted stream).
"""
from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).resolve().parent


def q(value: str) -> str:
    return "'" + value.replace("'", "''") + "'"


def conv(cid: str, *, title: str | None = None, last_updated: int = 0,
         data_changed_at: int = 0, selected: str | None = None,
         model: str | None = None, task: str | None = None,
         origin: str = "user", graduated: bool = False,
         draft: str = "", run_selected: str | None = None,
         unread: bool = False, pinned: bool = False) -> str:
    return (
        "INSERT INTO conversations (id, title, lastUpdated, dataChangedAt, "
        "selectedBranchesJson, modelId, taskId, origin, graduated, draftText, "
        "selectedRunBranchesJson, hasUnreadGeneration, isPinned) VALUES ("
        f"{q(cid)}, {q(title or cid)}, {last_updated}, {data_changed_at}, "
        f"{q(selected) if selected is not None else 'NULL'}, "
        f"{q(model) if model is not None else 'NULL'}, "
        f"{q(task) if task is not None else 'NULL'}, {q(origin)}, "
        f"{1 if graduated else 0}, {q(draft)}, "
        f"{q(run_selected) if run_selected is not None else 'NULL'}, "
        f"{1 if unread else 0}, {1 if pinned else 0})"
    )


def run(rid: str, cid: str, *, parent: str | None = None, status: str = "COMPLETED",
        active_slot: int | None = None, started_at: int = 0,
        last_checkpoint: int = 0, stop_requested: int | None = None,
        ended_at: int | None = 0, reason: str | None = "MODEL_COMPLETED",
        current_pass: int = 0, legacy: bool = False) -> str:
    return (
        "INSERT INTO runs (id, conversationId, parentRunId, status, activeSlot, startedAt, "
        "lastCheckpointAt, stopRequestedAt, endedAt, endReason, currentPass, legacyAmbiguous) "
        "VALUES ("
        f"{q(rid)}, {q(cid)}, {q(parent) if parent is not None else 'NULL'}, {q(status)}, "
        f"{active_slot if active_slot is not None else 'NULL'}, {started_at}, "
        f"{last_checkpoint}, {stop_requested if stop_requested is not None else 'NULL'}, "
        f"{ended_at if ended_at is not None else 'NULL'}, "
        f"{q(reason) if reason is not None else 'NULL'}, {current_pass}, "
        f"{1 if legacy else 0})"
    )


def message(mid: str, cid: str, rid: str, *, parent: str | None = None,
            text: str | None = None, timestamp: int = 0, sequence: int = 0,
            status: str = "SUCCESS", participant: str = "MODEL",
            images: str = "[]", token_count: int = 0,
            model: str | None = None) -> str:
    return (
        "INSERT INTO messages (id, conversationId, parentId, text, images, tokenCount, "
        "status, participant, timestamp, modelName, runId, runSequence) VALUES ("
        f"{q(mid)}, {q(cid)}, {q(parent) if parent is not None else 'NULL'}, "
        f"{q(text if text is not None else mid)}, {q(images)}, {token_count}, {q(status)}, "
        f"{q(participant)}, {timestamp}, {q(model) if model is not None else 'NULL'}, "
        f"{q(rid)}, {sequence})"
    )


def loop(cid: str, *, interval: int = 60000, prompt: str | None = None,
         next_fire: int = 1000, cycle: int = 0, max_cycles: int | None = None,
         active: bool = True, revision: int = 0) -> str:
    return (
        "INSERT INTO loops (conversationId, intervalMs, prompt, nextFireAt, cycleCount, "
        "maxCycles, active, revision) VALUES ("
        f"{q(cid)}, {interval}, {q(prompt) if prompt is not None else 'NULL'}, {next_fire}, "
        f"{cycle}, {max_cycles if max_cycles is not None else 'NULL'}, "
        f"{1 if active else 0}, {revision})"
    )


def task(tid: str, *, name: str | None = None, prompt: str | None = None,
         cron: str = "0 * * * *", next_run: int = 1000, enabled: bool = True,
         created: int = 0, last_run: int | None = None) -> str:
    return (
        "INSERT INTO tasks (id, name, prompt, cronExpr, nextRunAt, enabled, createdAt, "
        "lastRunAt) VALUES ("
        f"{q(tid)}, {q(name or tid)}, {q(prompt or tid)}, {q(cron)}, {next_run}, "
        f"{1 if enabled else 0}, {created}, "
        f"{last_run if last_run is not None else 'NULL'})"
    )


def scenario(*, seed: list[str], steps: list[dict]) -> dict:
    return {"seedSql": seed, "steps": steps}


def write(name: str, payload: dict) -> None:
    (HERE / "scenarios" / f"{name}.json").write_text(
        json.dumps(payload, indent=2, ensure_ascii=False) + "\n"
    )


def main() -> None:
    (HERE / "scenarios").mkdir(exist_ok=True)

    write("01-empty", scenario(seed=[], steps=[{"op": "readSnapshot"}]))

    write(
        "02-single-conversation",
        scenario(
            seed=[
                conv("c1", title="one", last_updated=10, data_changed_at=11,
                     selected='{"old":"m1"}', model="m0", task="t1",
                     draft="draft", run_selected='{"null":"r1"}', unread=True,
                     pinned=True),
                run("r1", "c1", started_at=10, ended_at=20),
                run("r2", "c1", parent="r1", started_at=30, ended_at=40),
                message("m1", "c1", "r1", text="first", timestamp=1, sequence=0),
                message("m2", "c1", "r1", parent="m1", text="second", timestamp=2,
                        sequence=1, images='["a.png","b.png"]', model="m0"),
                message("m3", "c1", "r2", parent="m2", text="third", timestamp=3,
                        sequence=0, token_count=7),
                loop("c1", prompt="continue", next_fire=500, cycle=2, max_cycles=8,
                     revision=3),
                task("t1", name="task one", prompt="do one", next_run=200,
                     created=1, last_run=3),
                task("t2", name="task two", prompt="do two", next_run=300,
                     created=2, enabled=False),
            ],
            steps=[{"op": "readSnapshot"}],
        ),
    )

    write(
        "03-multi-conversation",
        scenario(
            seed=[
                conv("c1", title="one"),
                conv("c2", title="two", unread=True),
                conv("c3", title="three", pinned=True),
                run("r1", "c1", started_at=1),
                run("r2", "c2", started_at=2),
                run("r3", "c3", started_at=3),
                message("m1", "c1", "r1", text="c1", timestamp=1),
                message("m2", "c2", "r2", text="c2", timestamp=2),
                message("m3", "c3", "r3", text="c3", timestamp=3),
                loop("c1", prompt="c1 loop"),
                loop("c2", prompt="c2 loop"),
                loop("c3", prompt="c3 loop"),
                task("t2", name="second", created=2),
                task("t1", name="first", created=1),
            ],
            steps=[
                {"op": "readSnapshot", "skipBody": ["c2"]},
                {"op": "readSnapshot", "skipBody": ["c1", "c3"]},
            ],
        ),
    )

    page_65 = [conv("c1", title="many"), run("r1", "c1", started_at=1)]
    page_65 += [
        message(f"m{i:03d}", "c1", "r1", text=f"message {i}", timestamp=100 + i,
                sequence=i - 1)
        for i in range(1, 66)
    ]
    write(
        "04-message-page-65",
        scenario(seed=page_65, steps=[{"op": "readSnapshot"}]),
    )

    page_64 = [conv("c1", title="boundary"), run("r1", "c1", started_at=1)]
    page_64 += [
        message(f"m{i:03d}", "c1", "r1", text=f"message {i}", timestamp=100 + i,
                sequence=i - 1)
        for i in range(1, 65)
    ]
    write(
        "05-message-page-boundary-64",
        scenario(seed=page_64, steps=[{"op": "readSnapshot"}]),
    )

    write(
        "06-skip-all-bodies",
        scenario(
            seed=[
                conv("c1"),
                conv("c2"),
                conv("c3"),
                run("r1", "c1", started_at=1),
                message("m1", "c1", "r1", timestamp=1),
                run("r2", "c2", started_at=2),
                message("m2", "c2", "r2", timestamp=2),
                run("r3", "c3", started_at=3),
                message("m3", "c3", "r3", timestamp=3),
                loop("c1"),
                loop("c2"),
                loop("c3"),
                task("t1", created=1),
            ],
            steps=[{"op": "readSnapshot", "skipBody": ["c1", "c2", "c3"]}],
        ),
    )

    write(
        "07-include-body-error",
        scenario(
            seed=[
                conv("c1"),
                conv("c2"),
                conv("c3"),
                run("r1", "c1", started_at=1),
                run("r2", "c2", started_at=2),
                run("r3", "c3", started_at=3),
                message("m1", "c1", "r1", timestamp=1),
                message("m2", "c2", "r2", timestamp=2),
                message("m3", "c3", "r3", timestamp=3),
                loop("c1"),
                loop("c2"),
                loop("c3"),
                task("t1", created=1),
            ],
            steps=[
                {"op": "readSnapshot", "failIncludeBody": "c2"},
                {"op": "readSnapshot"},
            ],
        ),
    )

    write(
        "08-on-record-error",
        scenario(
            seed=[
                conv("c1"),
                run("r1", "c1", started_at=1),
                message("m1", "c1", "r1", timestamp=1),
                message("m2", "c1", "r1", timestamp=2),
                message("m3", "c1", "r1", timestamp=3),
                loop("c1"),
                task("t1", created=1),
            ],
            steps=[
                {"op": "readSnapshot", "failOnRecordAt": 2},
                {"op": "readSnapshot"},
            ],
        ),
    )

    write(
        "09-order-ties",
        scenario(
            seed=[
                conv("c1"),
                conv("c2"),
                run("rB", "c1", started_at=10),
                run("rA", "c1", started_at=10),
                run("rC", "c1", started_at=5),
                message("m2", "c1", "rA", timestamp=10, sequence=2),
                message("m10", "c1", "rA", timestamp=10, sequence=1),
                message("m01", "c1", "rA", timestamp=10, sequence=1),
                message("m001", "c1", "rA", timestamp=10, sequence=1),
                message("m3", "c2", "rB", timestamp=3),
                loop("c1", revision=1),
                loop("c2", revision=2),
                task("t3", created=3),
                task("t1", created=1),
                task("t2", created=2),
            ],
            steps=[{"op": "readSnapshot"}],
        ),
    )

    write(
        "10-no-body-with-tasks",
        scenario(
            seed=[
                conv("c1"),
                conv("c2"),
                run("r1", "c1", started_at=1),
                message("m1", "c1", "r1", timestamp=1),
                loop("c1"),
                task("t3", name="three", created=3),
                task("t1", name="one", created=1),
                task("t2", name="two", created=2),
            ],
            steps=[{"op": "readSnapshot", "skipBody": ["c1", "c2"]}],
        ),
    )


if __name__ == "__main__":
    main()
