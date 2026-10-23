#!/usr/bin/env python3
"""Deterministic scenario generator for the new-chat-persist-dao stand.

The dataset targets every declaration of data/local/NewChatPersistDao.kt: the singleton Flow and
suspend read (present/absent), Room's @Upsert insert-then-update-by-primary-key algorithm
(including clearing columns to NULL), the unconditional singleton delete, and the null-safe `IS`
matching delete with all fields equal/unequal. It also pins NewChatPersistEntity's
`require(id == SINGLETON_ID)` construction guard.
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


def persist(identifier: int = 0, *, model_id: str | None = None,
            system_prompt_id: str | None = None,
            settings: str | None = None, draft_text: str = "",
            attachments: str | None = None) -> dict:
    return {
        "id": identifier,
        "modelId": model_id,
        "systemPromptId": system_prompt_id,
        "conversationSettingsJson": settings,
        "draftText": draft_text,
        "draftAttachments": attachments,
    }


def op(name: str, **arguments) -> dict:
    step = {"op": name}
    step.update(arguments)
    return step


def seed(rows: list[dict]) -> dict:
    return {"newChatPersist": rows}


def delete_matching(**arguments) -> dict:
    return op("deleteNewChatPersistIfMatches", **arguments)


def main() -> None:
    write("01-empty-then-upsert", {
        "seed": seed([]),
        "steps": [
            op("observeNewChatPersist"),
            op("getNewChatPersist"),
            op("upsertNewChatPersist", modelId="m1", systemPromptId=None,
               conversationSettingsJson='{"t":0.2}', draftText="hello",
               draftAttachments='["a.png"]'),
            op("observeNewChatPersist"),
            op("getNewChatPersist"),
            op("upsertNewChatPersist", modelId=None, systemPromptId="p2",
               conversationSettingsJson=None, draftText="", draftAttachments=None),
            op("getNewChatPersist"),
            op("observeNewChatPersist"),
            delete_matching(modelId=None, systemPromptId="p2",
                            conversationSettingsJson=None, draftText="",
                            draftAttachments=None),
            op("observeNewChatPersist"),
            op("getNewChatPersist"),
            op("deleteNewChatPersist"),
        ],
    })
    write("02-seeded-row-matching", {
        "seed": seed([persist(0, model_id="m1", system_prompt_id="p1",
                              settings='{"x":1}', draft_text="draft",
                              attachments='["a"]')]),
        "steps": [
            op("getNewChatPersist"),
            op("observeNewChatPersist"),
            delete_matching(modelId="m1", systemPromptId="p1",
                            conversationSettingsJson='{"x":1}', draftText="draft",
                            draftAttachments='["a"]'),
            op("getNewChatPersist"),
            delete_matching(modelId="m1", systemPromptId="p1",
                            conversationSettingsJson='{"x":1}', draftText="draft",
                            draftAttachments='["a"]'),
        ],
    })
    write("03-null-safe-matching", {
        "seed": seed([persist(0, draft_text="x")]),
        "steps": [
            delete_matching(modelId=None, systemPromptId=None,
                            conversationSettingsJson=None, draftText="y",
                            draftAttachments=None),
            op("getNewChatPersist"),
            delete_matching(modelId=None, systemPromptId=None,
                            conversationSettingsJson=None, draftText="x",
                            draftAttachments=None),
            op("getNewChatPersist"),
            op("deleteNewChatPersist"),
        ],
    })
    write("04-invalid-entity", {
        "seed": seed([]),
        "steps": [
            op("upsertNewChatPersist", id=1, draftText="bad"),
            op("upsertNewChatPersist", id=-1, draftText="worse"),
            op("observeNewChatPersist"),
            op("getNewChatPersist"),
        ],
    })
    write("05-upsert-clears-columns", {
        "seed": seed([persist(0, model_id="m1", system_prompt_id="p1",
                              settings='{"x":1}', draft_text="draft",
                              attachments='["a"]')]),
        "steps": [
            op("upsertNewChatPersist", modelId=None, systemPromptId=None,
               conversationSettingsJson=None, draftText="z", draftAttachments=None),
            op("getNewChatPersist"),
            op("observeNewChatPersist"),
            op("deleteNewChatPersist"),
            op("observeNewChatPersist"),
        ],
    })


if __name__ == "__main__":
    main()
