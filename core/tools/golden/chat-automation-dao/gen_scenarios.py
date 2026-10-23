#!/usr/bin/env python3
"""Deterministic scenario generator for the chat-automation-dao stand.

The dataset targets every declaration of data/local/ChatAutomationDao.kt: both observed Flow
queries and their suspend twins, Room's @Upsert for tasks and loops, the exact-match and
provider-prefix model-reference rewrites, the clock-change CAS updates for tasks and loops, the
loop deactivation CAS (revision bump, next-fire reset, max-cycle normalization), and every delete
shape (by id, by conversation, all rows).
"""
from __future__ import annotations

import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    os.makedirs(OUT, exist_ok=True)
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation(identifier: str) -> dict:
    return {"id": identifier}


def task(identifier: str, *, name: str | None = None, prompt: str | None = None,
         system_prompt: str | None = None, system_prompt_id: str | None = None,
         model_id: str | None = None, cron: str = "", run_at: int | None = None,
         next_run_at: int = 0, enabled: bool = True, created_at: int = 0,
         last_run_at: int | None = None) -> dict:
    row = {
        "id": identifier,
        "name": name if name is not None else identifier,
        "prompt": prompt if prompt is not None else "prompt-" + identifier,
        "cronExpr": cron,
        "nextRunAt": next_run_at,
        "enabled": enabled,
        "createdAt": created_at,
    }
    if system_prompt is not None:
        row["systemPrompt"] = system_prompt
    if system_prompt_id is not None:
        row["systemPromptId"] = system_prompt_id
    if model_id is not None:
        row["modelId"] = model_id
    if run_at is not None:
        row["runAt"] = run_at
    if last_run_at is not None:
        row["lastRunAt"] = last_run_at
    return row


def loop(conversation_id: str, *, interval_ms: int = 60000, prompt: str | None = None,
         next_fire_at: int = 0, cycle_count: int = 0, max_cycles: int | None = None,
         active: bool = True, revision: int = 0) -> dict:
    row = {
        "conversationId": conversation_id,
        "intervalMs": interval_ms,
        "nextFireAt": next_fire_at,
        "cycleCount": cycle_count,
        "active": active,
        "revision": revision,
    }
    if prompt is not None:
        row["prompt"] = prompt
    if max_cycles is not None:
        row["maxCycles"] = max_cycles
    return row


def seed(*, conversations: list[dict] | None = None, tasks: list[dict] | None = None,
         loops: list[dict] | None = None) -> dict:
    return {
        "conversations": conversations or [],
        "tasks": tasks or [],
        "loops": loops or [],
    }


def op(operation: str, **arguments) -> dict:
    step = {"op": operation}
    step.update(arguments)
    return step


def update_task_cas(identifier: str, cron: str, expected: int, replacement: int) -> dict:
    return op("updateTaskNextRunAtIfUnchanged", id=identifier, expectedCronExpr=cron,
              expectedNextRunAt=expected, replacementNextRunAt=replacement)


def update_loop_cas(conversation_id: str, revision: int, cycle: int, interval_ms: int,
                    expected: int, replacement: int) -> dict:
    return op("updateLoopNextFireAtIfUnchanged", conversationId=conversation_id,
              expectedRevision=revision, expectedCycleCount=cycle,
              expectedIntervalMs=interval_ms, expectedNextFireAt=expected,
              replacementNextFireAt=replacement)


def deactivate_loop_cas(conversation_id: str, revision: int, cycle: int, interval_ms: int,
                        expected: int, max_cycles: int) -> dict:
    return op("deactivateLoopRowIfUnchanged", conversationId=conversation_id,
              expectedRevision=revision, expectedCycleCount=cycle,
              expectedIntervalMs=interval_ms, expectedNextFireAt=expected,
              normalizedMaxCycles=max_cycles)


def main() -> None:
    write("01-tasks-read-and-upsert", {
        "seed": seed(tasks=[
            task("t-old", name="old", system_prompt="sys", model_id="openai:gpt-4",
                 cron="0 0 * * *", next_run_at=1000, created_at=100, last_run_at=50),
            task("t-new", name="new", system_prompt_id="sp-1", cron="", run_at=5000,
                 next_run_at=0, enabled=False, created_at=300),
            task("t-mid", name="mid", model_id="anthropic:claude", cron="*/5 * * * *",
                 next_run_at=2000, created_at=200, last_run_at=150),
        ]),
        "steps": [
            op("getAllTasks"),
            op("getAllTasksList"),
            op("getTask", id="t-mid"),
            op("getTask", id="missing"),
            op("getEnabledTasks"),
            op("upsertTask", id="t-new", name="new name", prompt="p-new2",
               systemPrompt="sys2", systemPromptId=None, modelId="local:qwen",
               cronExpr="0 12 * * *", runAt=None, nextRunAt=7000, enabled=True,
               createdAt=300, lastRunAt=600),
            op("getTask", id="t-new"),
            op("upsertTask", id="t-insert", name="inserted", prompt="pi",
               systemPromptId="sp-2", modelId="openai:gpt-4o", cronExpr="0 0 1 * *",
               nextRunAt=9000, enabled=True, createdAt=400),
            op("getAllTasksList"),
            op("getEnabledTasks"),
        ],
    })
    write("02-task-model-references", {
        "seed": seed(tasks=[
            task("t1", model_id="openai:gpt-4", created_at=1),
            task("t2", model_id="openai:gpt-4o", created_at=2),
            task("t3", model_id="anthropic:claude", created_at=3),
            task("t4", created_at=4),
            task("t5", model_id="other", created_at=5),
            task("t6", model_id="openai", created_at=6),
            task("t7", model_id="openaix:foo", created_at=7),
        ]),
        "steps": [
            op("replaceTaskModelReferences", oldModelId="openai:gpt-4",
               newModelId="local:model"),
            op("replaceTaskModelReferences", oldModelId="openai:gpt-4", newModelId=None),
            op("replaceTaskModelReferences", oldModelId="missing", newModelId="x"),
            op("renameTaskProviderModelReferences", oldProvider="openai", newProvider="azure"),
            op("renameTaskProviderModelReferences", oldProvider="azure", newProvider="openai"),
            op("renameTaskProviderModelReferences", oldProvider="openai", newProvider="x"),
            op("getAllTasksList"),
        ],
    })
    write("03-task-clock-cas", {
        "seed": seed(tasks=[
            task("t1", cron="0 0 * * *", next_run_at=100, created_at=1),
            task("t2", cron="0 0 * * *", next_run_at=100, enabled=False, created_at=2),
        ]),
        "steps": [
            update_task_cas("t1", "0 0 * * *", 100, 200),
            op("getTask", id="t1"),
            update_task_cas("t1", "0 0 * * *", 100, 200),
            update_task_cas("t1", "0 0 * * *", 200, 300),
            update_task_cas("t2", "0 0 * * *", 100, 200),
            update_task_cas("t1", "bad cron", 300, 400),
            update_task_cas("missing", "0 0 * * *", 300, 400),
            op("deleteTask", id="t1"),
            op("getTask", id="t1"),
            op("deleteTask", id="t1"),
            op("deleteAllTasks"),
            op("getAllTasksList"),
        ],
    })
    write("04-loops-read-and-upsert", {
        "seed": seed(
            conversations=[conversation("c1"), conversation("c2"), conversation("c3")],
            loops=[
                loop("c1", prompt="continue", next_fire_at=5000, cycle_count=1,
                     max_cycles=10, revision=2),
                loop("c2", interval_ms=30000, active=False),
            ],
        ),
        "steps": [
            op("getLoop", conversationId="c1"),
            op("getLoop", conversationId="missing"),
            op("getActiveLoops"),
            op("observeActiveLoops"),
            op("getAllLoopsList"),
            op("getLoopsForConversationSnapshot", conversationId="c2"),
            op("upsertLoopRow", conversationId="c2", intervalMs=30000, prompt="p2",
               nextFireAt=7000, cycleCount=3, maxCycles=5, active=True, revision=1),
            op("observeActiveLoops"),
            op("upsertLoopRow", conversationId="c3", intervalMs=10000, prompt=None,
               nextFireAt=9000, cycleCount=0, maxCycles=None, active=True, revision=0),
            op("getAllLoopsList"),
        ],
    })
    write("05-loop-clock-cas", {
        "seed": seed(
            conversations=[conversation("c1"), conversation("c2")],
            loops=[
                loop("c1", prompt="continue", next_fire_at=5000, cycle_count=1,
                     max_cycles=10, revision=2),
                loop("c2", interval_ms=30000, active=False),
            ],
        ),
        "steps": [
            update_loop_cas("c1", 2, 1, 60000, 5000, 9000),
            op("getLoop", conversationId="c1"),
            update_loop_cas("c1", 2, 1, 60000, 5000, 9000),
            update_loop_cas("c1", 2, 1, 60000, 9000, 12000),
            update_loop_cas("c1", 3, 1, 60000, 12000, 15000),
            update_loop_cas("c2", 0, 0, 30000, 0, 1000),
            deactivate_loop_cas("c1", 2, 1, 60000, 12000, 7),
            op("getLoop", conversationId="c1"),
            deactivate_loop_cas("c1", 2, 1, 60000, 12000, 7),
            op("deleteLoopRow", conversationId="c1"),
            op("deleteLoopRow", conversationId="c1"),
            op("deleteAllLoops"),
            op("getAllLoopsList"),
        ],
    })
    write("06-loop-upsert-clears", {
        "seed": seed(
            conversations=[conversation("c1")],
            loops=[loop("c1", prompt="p", next_fire_at=2000, cycle_count=2,
                        max_cycles=9, revision=4)],
        ),
        "steps": [
            op("upsertLoopRow", conversationId="c1", intervalMs=1000, prompt=None,
               nextFireAt=0, cycleCount=2, maxCycles=None, active=False, revision=5),
            op("getLoop", conversationId="c1"),
            op("upsertLoopRow", conversationId="c1", intervalMs=2000, prompt="x",
               nextFireAt=5000, cycleCount=0, maxCycles=3, active=True, revision=6),
            op("getLoop", conversationId="c1"),
            op("deleteAllLoops"),
            op("getAllLoopsList"),
        ],
    })


if __name__ == "__main__":
    main()
