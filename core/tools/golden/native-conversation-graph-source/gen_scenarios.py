#!/usr/bin/env python3
"""Deterministic scenario generator for NativeConversationGraphSource.

Scenarios are the entry surface: a version, the archive's entries (name -> content), which
streams are null or drain-faulted, and the steps to run (full open, or a partial read + close).
Conversation item entry names use the app's conversationEntry(id) (UTF-8 hex of the id).
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
    return json.dumps(
        {"conversations": list(conversations)},
        separators=(",", ":"),
        ensure_ascii=False,
    )


def item(conversation_id: str, changed: int, payload: dict) -> str:
    base = {
        "conversations": [
            {"id": conversation_id, "title": "t", "lastUpdated": 1, "dataChangedAt": changed}
        ],
        "runs": [],
        "messages": [],
        "loops": [],
    }
    base.update(payload)
    return json.dumps(base, separators=(",", ":"), ensure_ascii=False)


def write(name: str, *, version: int, entries: dict, steps, stream_null=None, drain_faults=None):
    scenario = {"version": version, "entries": entries, "steps": steps}
    if stream_null:
        scenario["streamNull"] = stream_null
    if drain_faults:
        scenario["drainFaults"] = drain_faults
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, indent=2, ensure_ascii=False)
        handle.write("\n")


def main() -> None:
    os.makedirs(OUT, exist_ok=True)
    for name in os.listdir(OUT):
        if name.endswith(".json"):
            os.remove(os.path.join(OUT, name))

    legacy_graph = json.dumps(
        {"conversations": [{"id": "c1"}], "tasks": [{"id": "t1"}]},
        separators=(",", ":"),
    )
    write(
        "01-legacy-v4",
        version=4,
        entries={"conversations.json": legacy_graph},
        steps=[{"op": "open"}, {"op": "open-partial"}],
    )
    write(
        "02-legacy-missing",
        version=4,
        entries={"other.json": "{}"},
        steps=[{"op": "open"}],
    )
    write(
        "03-legacy-stream-null",
        version=4,
        entries={"conversations.json": legacy_graph},
        steps=[{"op": "open"}],
        stream_null=["conversations.json"],
    )

    c1 = conversation_entry("c1")
    c2 = conversation_entry("c2")

    write(
        "04-index-missing",
        version=5,
        entries={TASKS: json.dumps({"tasks": []}, separators=(",", ":"))},
        steps=[{"op": "open"}],
    )
    write(
        "05-index-missing-field",
        version=5,
        entries={INDEX: "{}"},
        steps=[{"op": "open"}],
    )
    write(
        "06-index-broken-json",
        version=5,
        entries={INDEX: "not json"},
        steps=[{"op": "open"}],
    )
    write(
        "07-duplicate-id",
        version=5,
        entries={INDEX: index(
            {"id": "c1", "dataChangedAt": 1, "entry": c1},
            {"id": "c1", "dataChangedAt": 2, "entry": conversation_entry("c1x")},
        )},
        steps=[{"op": "open"}],
    )
    write(
        "08-entry-mismatch",
        version=5,
        entries={INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": "conv/items/dead.json"})},
        steps=[{"op": "open"}],
    )
    write(
        "09-item-missing",
        version=5,
        entries={INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1})},
        steps=[{"op": "open"}],
    )
    write(
        "10-tasks-missing",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: item("c1", 1, {}),
        },
        steps=[{"op": "open"}],
    )

    rich = item("c1", 7, {
        # The conversation object itself carries a nested object so copy_value's recursion and
        # Android's number re-emission inside a known field are both pinned.
        "conversations": [{
            "id": "c1",
            "title": "t",
            "lastUpdated": 1,
            "dataChangedAt": 7,
            "conversationSettings": {"temperature": 0.7, "topP": None, "maxTokens": 512},
        }],
        "runs": [{"id": "r1", "conversationId": "c1", "currentPass": 0}],
        "messages": [{
            "id": "m1",
            "conversationId": "c1",
            "text": "Quote \\\" backslash \\\\ slash \\/ newline \\n tab \\t cafe \u00e9 \U0001f600",
            "tokenCount": 12,
            "duration": 1.0E10,
            # Every number shape copy_value must re-emit through Kotlin's toLongOrNull /
            # parseDouble and Android's Double.toString: decimal stays decimal, exponents
            # cross Java's < -3 / >= 7 rule, overflow goes through the double path, and
            # -0.0 keeps its sign.
            "ratio": 0.7,
            "sci": 1e5,
            "huge": 99999999999999999999,
            "negZero": -0.0,
            "small": 0.0001,
            "frac": 123.456,
            "floatOne": 1.0,
            "rawDouble": 10000000000.0,
            "unknownInside": {"deep": [True, None, "x"]},
        }],
        "loops": [{"conversationId": "c1", "intervalMs": 60000, "prompt": None}],
        "tasks": [{"id": "ignored", "name": "dropped"}],
        "extra": [[1, 2], {"skip": [True, False]}, "stray"],
        "numbers": {
            "zero": 0,
            "negZero": -0,
            "seven": -7,
            "floatOne": 1.0,
            "temp": 0.7,
            "sci": 1e5,
            "big": 99999999999999999999,
            "small": 1.0E-4,
            "frac": 123.456,
            "e10": 1.0E10,
        },
        "conversationSettings": {"temperature": 0.7, "topP": None, "maxTokens": 512},
        "nested": {"deep": [1, [2, [3, {"leaf": "value"}]]]},
    })
    second = item("c2", 9, {
        "messages": [{"id": "m2", "conversationId": "c2", "text": "second"}],
    })
    tasks_entry = json.dumps(
        {"tasks": [{"id": "t1", "name": "Daily"}], "padding": {"ignored": [1, 2]}},
        separators=(",", ":"),
        ensure_ascii=False,
    )
    write(
        "11-split-happy",
        version=5,
        entries={
            INDEX: index(
                {"id": "c1", "dataChangedAt": 7, "entry": c1},
                {"id": "c2", "dataChangedAt": 9, "entry": c2},
            ),
            c1: rich,
            c2: second,
            TASKS: tasks_entry,
        },
        steps=[{"op": "open"}, {"op": "open-partial"}],
    )
    write(
        "12-empty-conversations",
        version=5,
        entries={
            INDEX: index(),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )

    write(
        "13-item-unquoted-name",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: '{"conversations": [{id: "c1"}], "runs": [], "messages": [], "loops": []}',
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )
    write(
        "14-item-truncated",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: '{"conversations": [{"id": "c1", "title": "unfinished"',
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )
    write(
        "15-item-raw-control-char",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: '{"conversations": [{"id": "c1", "title": "a\u0007b"}], "runs": [], "messages": [], "loops": []}',
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )
    write(
        "16-drain-fault",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: item("c1", 1, {}),
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
        drain_faults={TASKS: "Read error: CRC mismatch"},
    )
    write(
        "17-item-trailing-bytes",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: item("c1", 1, {}) + "\nTRAILING GARBAGE",
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )


    write(
        "05b-index-unknown-key",
        version=5,
        entries={INDEX: '{"conversations": [], "bogus": 1}'},
        steps=[{"op": "open"}],
    )
    write(
        "05c-index-wrong-type",
        version=5,
        entries={INDEX: '{"conversations": "nope"}'},
        steps=[{"op": "open"}],
    )
    write(
        "05d-index-item-missing-field",
        version=5,
        entries={INDEX: '{"conversations": [{"id": "c1"}]}'},
        steps=[{"op": "open"}],
    )
    write(
        "18-item-field-not-array",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: '{"conversations": 5, "runs": [], "messages": [], "loops": []}',
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )
    write(
        "19-item-not-object",
        version=5,
        entries={
            INDEX: index({"id": "c1", "dataChangedAt": 1, "entry": c1}),
            c1: '[]',
            TASKS: json.dumps({"tasks": []}, separators=(",", ":")),
        },
        steps=[{"op": "open"}],
    )

    print(f"Wrote {len([n for n in os.listdir(OUT) if n.endswith('.json')])} scenarios to {OUT}")


if __name__ == "__main__":
    main()
