#!/usr/bin/env python3
"""Deterministic scenario generator for the native backup v5 stand."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def conversation_entry(identifier: str) -> str:
    """NativeBackupFormat.conversationEntry(id): the hex-derived archive name."""
    return "conv/items/" + identifier.encode("utf-8").hex() + ".json"


def entry(identifier: str, data_changed_at: int, media: list[str] | None = None) -> dict:
    return {"id": identifier, "dataChangedAt": data_changed_at,
            "entry": conversation_entry(identifier),
            "mediaEntries": media or []}


def item(identifier: str, data_changed_at: int, conversation: str,
         runs: list[str] | None = None, messages: list[str] | None = None,
         loops: list[str] | None = None, media: list[str] | None = None) -> dict:
    return {"id": identifier, "dataChangedAt": data_changed_at,
            "conversation": conversation, "runs": runs or [],
            "messages": messages or [], "loops": loops or [],
            "mediaEntries": media or []}


def baseline(identifier: str, manifest: dict | None = None, items: list[dict] | None = None,
             media: list[dict] | None = None, index: list[dict] | None = None,
             manifest_text: str | None = None, index_text: str | None = None,
             dump: bool = True) -> dict:
    step = {"op": "createBaseline", "file": identifier, "dump": dump,
            "manifest": manifest or {
                "agora_export_version": 6, "incremental_baseline": 1,
                "categories": ["conversations"],
            },
            "items": items or [], "media": media or []}
    if index is not None:
        step["index"] = index
    if manifest_text is not None:
        step["manifestText"] = manifest_text
    if index_text is not None:
        step["indexText"] = index_text
    return step


def open_baseline(identifier: str) -> dict:
    return {"op": "openOrNull", "file": identifier}


def reuse(identifier: str, conversation_id: str, data_changed_at: int) -> dict:
    return {"op": "canReuse", "file": identifier, "id": conversation_id,
            "dataChangedAt": data_changed_at}


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # 01 — entry assembly: separators, record joins, empty record lists, media sorting.
    write("01-write-entries", {
        "steps": [
            {"op": "writeConversationEntry", "file": "out",
             "conversationId": "conv-1", "dataChangedAt": 100,
             "conversation": '{"id":"conv-1","title":"one"}',
             "runs": ['{"id":"r1"}', '{"id":"r2"}'],
             "messages": ['{"id":"m1"}'],
             "loops": [],
             "mediaEntries": ["media/images/b.png", "media/images/a.png"]},
            {"op": "writeConversationEntry", "file": "out",
             "conversationId": "conv2", "dataChangedAt": 200,
             "conversation": '{"id":"conv2"}',
             "runs": [], "messages": ['{"id":"m2"}', '{"id":"m3"}'],
             "loops": ['{"conversationId":"conv2"}'], "mediaEntries": []},
            {"op": "writeTasksEntry", "file": "out",
             "tasks": ['{"id":"t1"}', '{"id":"t2"}', '{"id":"t3"}']},
            {"op": "writeConversationIndex", "file": "out",
             "entries": [entry("conv-1", 100, ["media/images/a.png", "media/images/b.png"]),
                         entry("conv2", 200)]},
            {"op": "writeTasksEntry", "file": "empty-tasks", "tasks": []},
        ],
    })

    # 02 — a valid baseline opens, its index round trips, and lookups answer hit and miss.
    write("02-baseline-open-valid", {
        "steps": [
            baseline("b0", items=[
                item("c1", 10, '{"id":"c1","title":"first"}', runs=['{"id":"r1"}'],
                     media=["media/images/x.png"]),
                item("c2", 20, '{"id":"c2"}', messages=['{"id":"m2"}']),
            ], media=[{"name": "media/images/x.png", "content": "PNGDATA"}]),
            open_baseline("b0"),
            {"op": "indexEntry", "file": "b0", "id": "c1"},
            {"op": "indexEntry", "file": "b0", "id": "missing"},
            open_baseline("never-created"),
        ],
    })

    # 03 — every validation in openOrNull: manifest version, baseline revision, categories,
    # index JSON, unique ids, derived entry names, and item/media existence.
    write("03-baseline-open-invalid", {
        "steps": [
            baseline("wrong-version", manifest={"agora_export_version": 5,
                                                "incremental_baseline": 1,
                                                "categories": ["conversations"]},
                     items=[item("c1", 10, '{"id":"c1"}')]),
            open_baseline("wrong-version"),
            baseline("wrong-revision", manifest={"agora_export_version": 6,
                                                 "incremental_baseline": 2,
                                                 "categories": ["conversations"]},
                     items=[item("c1", 10, '{"id":"c1"}')]),
            open_baseline("wrong-revision"),
            baseline("wrong-categories", manifest={"agora_export_version": 6,
                                                   "incremental_baseline": 1,
                                                   "categories": ["settings"]},
                     items=[item("c1", 10, '{"id":"c1"}')]),
            open_baseline("wrong-categories"),
            baseline("duplicate-ids", items=[item("c1", 10, '{"id":"c1"}')],
                     index=[entry("c1", 10), entry("c1", 11)]),
            open_baseline("duplicate-ids"),
            baseline("wrong-entry-name", items=[item("c1", 10, '{"id":"c1"}')],
                     index=[{"id": "c1", "dataChangedAt": 10,
                             "entry": "conv/items/wrong.json", "mediaEntries": []}]),
            open_baseline("wrong-entry-name"),
            baseline("missing-item", items=[],
                     index=[entry("c1", 10)]),
            open_baseline("missing-item"),
            baseline("missing-media", items=[item("c1", 10, '{"id":"c1"}',
                                                  media=["media/gone.png"])]),
            open_baseline("missing-media"),
            baseline("index-not-json", items=[item("c1", 10, '{"id":"c1"}')],
                     index_text="not json at all"),
            open_baseline("index-not-json"),
            baseline("manifest-not-json", items=[item("c1", 10, '{"id":"c1"}')],
                     manifest_text="oops"),
            open_baseline("manifest-not-json"),
        ],
    })

    # 04 — canReuse: a timestamp match over intact entries reuses; anything else refuses.
    write("04-can-reuse", {
        "steps": [
            baseline("b0", items=[
                item("c1", 100, '{"id":"c1"}', media=["media/images/x.png"]),
                item("c2", 200, '{"id":"c2"}'),
            ], media=[{"name": "media/images/x.png", "content": "PNGDATA"}]),
            reuse("b0", "c1", 100),
            reuse("b0", "c2", 200),
            reuse("b0", "c1", 999),
            reuse("b0", "unknown", 100),
            reuse("never-created", "c1", 100),
        ],
    })

    # 05 — the integrity check: a damaged media entry poisons its conversation, a damaged
    # conversation poisons only itself, and untouched conversations still reuse.
    write("05-can-reuse-damaged", {
        "steps": [
            baseline("b0", items=[
                item("c1", 100, '{"id":"c1"}', media=["media/images/x.png"]),
            ], media=[{"name": "media/images/x.png", "content": "PNGDATA"}]),
            {"op": "corruptEntry", "file": "b0", "name": "media/images/x.png"},
            reuse("b0", "c1", 100),
            baseline("b1", items=[
                item("c1", 100, '{"id":"c1"}'),
                item("c2", 200, '{"id":"c2"}'),
            ]),
            {"op": "corruptEntry", "file": "b1", "name": conversation_entry("c1")},
            reuse("b1", "c1", 100),
            reuse("b1", "c2", 200),
        ],
    })

    # 06 — raw copies: exact entry carry-over, media deduplication across calls, the caller's
    # pre-seeded set, and the missing-entry failure text.
    write("06-copy-raw", {
        "steps": [
            baseline("b0", items=[
                item("c1", 100, '{"id":"c1"}', runs=['{"id":"r1"}'],
                     media=["media/images/x.png"]),
            ], media=[{"name": "media/images/x.png", "content": "PNGDATA"}]),
            {"op": "copyRaw", "file": "out", "baseline": "b0",
             "name": conversation_entry("c1")},
            {"op": "copyRaw", "file": "out", "baseline": "b0",
             "name": "media/images/x.png"},
            {"op": "copyRaw", "file": "out", "baseline": "b0",
             "name": "media/missing.png"},
            {"op": "copyConversationFromBaseline", "file": "out2", "baseline": "b0",
             "entry": entry("c1", 100, ["media/images/x.png"]), "copiedMedia": []},
            {"op": "copyConversationFromBaseline", "file": "out2", "baseline": "b0",
             "entry": entry("c1", 100, ["media/images/x.png"])},
            {"op": "copyConversationFromBaseline", "file": "out3", "baseline": "b0",
             "entry": entry("c1", 100, ["media/images/x.png"]),
             "copiedMedia": ["media/pre-seeded.png"]},
        ],
    })

    # 07 — the incremental shape: copied baseline entries alongside freshly assembled ones.
    write("07-mixed-archive", {
        "steps": [
            baseline("b0", items=[
                item("c1", 100, '{"id":"c1"}', runs=['{"id":"r1"}'],
                     media=["media/images/x.png"]),
            ], media=[{"name": "media/images/x.png", "content": "PNGDATA"}]),
            {"op": "copyConversationFromBaseline", "file": "out", "baseline": "b0",
             "entry": entry("c1", 100, ["media/images/x.png"]), "copiedMedia": []},
            {"op": "writeConversationEntry", "file": "out",
             "conversationId": "c2", "dataChangedAt": 300,
             "conversation": '{"id":"c2","title":"fresh"}',
             "runs": ['{"id":"r2"}'], "messages": ['{"id":"m2"}'],
             "loops": [], "mediaEntries": []},
            {"op": "writeTasksEntry", "file": "out", "tasks": ['{"id":"t1"}']},
            {"op": "writeConversationIndex", "file": "out",
             "entries": [entry("c1", 100, ["media/images/x.png"]), entry("c2", 300)]},
        ],
    })

    # 08 — a conversation with no runs, messages or loops still produces every separator.
    write("08-empty-records", {
        "steps": [
            baseline("b0", items=[item("c9", 5, '{"id":"c9"}')]),
            open_baseline("b0"),
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
