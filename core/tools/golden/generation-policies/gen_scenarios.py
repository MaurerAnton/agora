#!/usr/bin/env python3
"""Deterministic scenario generator for the generation policies."""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

# 2024-05-06T07:08:09Z in milliseconds (a Monday, 07:08:09 UTC).
MONDAY = 1714979289000


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def message(message_id: str, participant: str = "USER", *, parent=None, text: str = "",
            timestamp: int = MONDAY, model=None, images=None, segments=None,
            run=None, source=None, status: str = "SUCCESS") -> dict:
    value = {"id": message_id, "participant": participant, "text": text,
             "timestamp": timestamp, "status": status}
    if parent is not None:
        value["parentId"] = parent
    if model is not None:
        value["modelName"] = model
    if images is not None:
        value["images"] = images
    if segments is not None:
        value["segments"] = segments
    if run is not None:
        value["runId"] = run
    if source is not None:
        value["source"] = source
    return value


def segment(kind: str, content: str = "", **extra) -> dict:
    value = {"type": kind, "content": content}
    value.update(extra)
    return value


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    base = [
        message("m1", "USER", text="question", parent=None),
        message("m2", "MODEL", text="answer", parent="m1", model="OpenAI:gpt-5.4"),
    ]

    write("01-policies-base", {
        "messages": base,
        "segments": [
            segment("thought", "thinking", signature="sig", signatureProvider="openai",
                    durationMs=1200),
            segment("answer", "partial"),
            segment("tool", "tool output"),
        ],
        "compactorRounds": [
            {"runId": "run-1", "segments": [
                segment("thought", "first thought", signature="sig1"),
                segment("answer", "answer one"),
            ]},
            {"runId": "run-1", "segments": [
                segment("thought", "first thought", signature="sig1"),
                segment("thought", "second thought", signature="sig2"),
                segment("answer", "answer two"),
            ]},
            {"runId": "run-1", "segments": [
                segment("thought", "first thought", signature="sig1"),
                segment("thought", "second thought", signature="sig2"),
                segment("answer", "answer two"),
            ]},
            {"runId": "run-2", "segments": [
                segment("thought", "other run", signature="sig3"),
            ]},
        ],
        "templates": {
            "userPrepend": "<user> ",
            "userPostpend": " {sent_time} {sent_date} {message_model_id}",
            "assistantPrepend": "",
            "assistantPostpend": " [{sent_date}]",
        },
        "automaticSource": {
            "kind": "ask_user",
            "askUser": [
                {"question": "which one?", "answer": "the first"},
                {"question": "and now?", "answer": None},
            ],
            "text": "raw text",
        },
        "projection": {"includeImages": True, "initialUserPrompt": "start here"},
    })

    # The templated variants: every placeholder and every role.
    write("02-policies-template-variants", {
        "messages": [
            message("m1", "USER", text="question"),
            message("m2", "MODEL", text="answer", parent="m1", model="gpt-4o"),
            message("m3", "USER", text="", parent="m2"),
            message("tool_1", "MODEL", text="tool row", parent="m2"),
            message("result_1", "MODEL", text="tool result", parent="tool_1"),
            message("compact_1", "MODEL", text="summary", parent="m2"),
            message("m4", "USER", text="second", parent="m2"),
        ],
        "segments": [],
        "compactorRounds": [],
        "templates": {
            "userPrepend": "PRE {sent_time}|",
            "userPostpend": "|POST {sent_date}|",
            "assistantPrepend": "A",
            "assistantPostpend": "B {message_model_id}",
        },
        "projection": {"includeImages": False},
    })

    # The automatic-source projection (task / loop / ask_user envelopes).
    write("03-policies-automatic-sources", {
        "messages": [
            message("m1", "USER", text="typed by the user"),
            message("m2", "USER", text="task prompt",
                    source={"kind": "task", "askUser": []}),
            message("m3", "USER", text="loop prompt",
                    source={"kind": "loop", "askUser": []}),
            message("m4", "USER", text="answers",
                    source={"kind": "ask_user", "askUser": [
                        {"question": "q1", "answer": "a1"},
                        {"question": "q2", "answer": None},
                    ]}),
            message("m5", "MODEL", text="model reply",
                    source={"kind": "task", "askUser": []}),
        ],
        "segments": [],
        "compactorRounds": [],
        "automaticSource": {"kind": "task", "askUser": [], "text": "task & <text>"},
        "projection": {"includeImages": True},
    })

    # Images and the initial prompt.
    write("04-policies-images-and-initial-prompt", {
        "messages": [
            message("m1", "USER", text="look", images=["a.png", "b.png"]),
            message("m2", "MODEL", text="answer", parent="m1", images=["c.png"]),
        ],
        "segments": [],
        "compactorRounds": [],
        "templates": {"userPrepend": "U", "userPostpend": ""},
        "projection": {"includeImages": False, "initialUserPrompt": "extra prompt"},
    })

    # The compactor edge cases.
    write("05-policies-compactor-edges", {
        "messages": base,
        "segments": [],
        "compactorRounds": [
            {"runId": "r", "segments": [segment("answer", "no thoughts")]},
            {"runId": "r", "segments": [
                segment("thought", "t1"),
                segment("thought", "t1"),
            ]},
            {"runId": "r", "segments": [
                segment("thought", "t1"),
                segment("thought", "t2"),
            ]},
            {"runId": "r", "segments": [
                segment("thought", "t1"),
                segment("thought", "t2"),
                segment("thought", "t3"),
            ]},
        ],
    })

    # Fuzz.
    for index in range(6):
        rng = random.Random(21000 + index)
        messages = []
        for position in range(rng.randint(1, 5)):
            messages.append(message(
                "m%d" % position,
                rng.choice(["USER", "MODEL", "MODEL"]),
                parent=("m%d" % rng.randint(0, position - 1)) if position > 0 else None,
                text=rng.choice(["", "text", "text 🙂", "{sent_time}"]),
                timestamp=rng.choice([MONDAY, 0, MONDAY + 1]),
                model=rng.choice([None, "gpt-4o", "OpenAI:gpt-5.4"]),
                images=rng.choice([None, [], ["a.png"]]),
                source=rng.choice([None, {"kind": "task", "askUser": []}]),
            ))
        rounds = []
        for _ in range(rng.randint(0, 4)):
            rounds.append({
                "runId": rng.choice(["r1", "r2"]),
                "segments": [segment(rng.choice(["thought", "answer", "tool"]),
                                    rng.choice(["", "x", "x"]))
                             for _ in range(rng.randint(0, 3))],
            })
        write("90-fuzz-%02d" % index, {
            "messages": messages,
            "segments": [segment("thought", "s", signature="sig", durationMs=5)],
            "compactorRounds": rounds,
            "templates": {
                "userPrepend": rng.choice(["", "U"]),
                "userPostpend": rng.choice(["", " {sent_time}"]),
            },
            "projection": {"includeImages": rng.choice([True, False])},
        })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
