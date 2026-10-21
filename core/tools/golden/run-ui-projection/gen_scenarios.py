#!/usr/bin/env python3
"""Deterministic scenario generator for the Run UI projection (message generation boundaries)."""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, visible: list, all_messages: list) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump({"visibleMessages": visible, "allMessages": all_messages},
                  handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def message(message_id: str, participant: str = "USER", *, parent: str | None = None,
            text: str = "", run: str | None = None, timestamp: int = 0,
            status: str = "SUCCESS") -> dict:
    value = {"id": message_id, "participant": participant, "text": text,
             "timestamp": timestamp, "status": status}
    if parent is not None:
        value["parentId"] = parent
    if run is not None:
        value["runId"] = run
    return value


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-single-user", [message("m1", "USER", text="hi")],
          [message("m1", "USER", text="hi")])
    write("02-user-assistant", [
        message("m1", "USER", text="hi", timestamp=1),
        message("m2", "MODEL", parent="m1", text="hello", timestamp=2),
    ], [
        message("m1", "USER", text="hi", timestamp=1),
        message("m2", "MODEL", parent="m1", text="hello", timestamp=2),
    ])
    write("03-edited-branches", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "USER", parent="m0", text="edited question", timestamp=2),
        message("m3", "MODEL", parent="m2", text="answer", timestamp=3),
    ], [
        message("m0", "USER", text="question", timestamp=1),
        message("m1", "USER", parent="m0", text="edited question", timestamp=2),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=3),
    ])
    write("04-regenerated-branches", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="first", timestamp=2),
        message("m3", "MODEL", parent="m1", text="second", timestamp=3),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="first", timestamp=2),
        message("m3", "MODEL", parent="m1", text="second", timestamp=3),
    ])
    write("05-protocol-rows", [
        message("m1", "USER", text="question", timestamp=1),
        message("tool_1", "MODEL", parent="m1", text="tool call", timestamp=2),
        message("result_1", "MODEL", parent="tool_1", text="tool result", timestamp=3),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=4),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("tool_1", "MODEL", parent="m1", text="tool call", timestamp=2),
        message("result_1", "MODEL", parent="tool_1", text="tool result", timestamp=3),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=4),
    ])
    write("06-compact-row", [
        message("m1", "USER", text="question", timestamp=1),
        message("compact_1", "MODEL", parent="m1", text="summary", timestamp=2),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=3),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("compact_1", "MODEL", parent="m1", text="summary", timestamp=2),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=3),
    ])
    write("07-run-groups", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="a", run="run-1", timestamp=2),
        message("m3", "MODEL", parent="m1", text="b", run="run-2", timestamp=3),
        message("m4", "MODEL", parent="m1", text="c", run="run-2", timestamp=4),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="a", run="run-1", timestamp=2),
        message("m3", "MODEL", parent="m1", text="b", run="run-2", timestamp=3),
        message("m4", "MODEL", parent="m1", text="c", run="run-2", timestamp=4),
    ])
    write("08-duplicate-ids", [
        message("m1", "USER", text="question", timestamp=1),
        message("m1", "USER", text="question", timestamp=1),
    ], [
        message("m1", "USER", text="question", timestamp=1),
    ])
    write("09-empty-text", [
        message("m1", "USER", text="", timestamp=1),
        message("m2", "MODEL", parent="m1", text="", timestamp=2),
    ], [
        message("m1", "USER", text="", timestamp=1),
        message("m2", "MODEL", parent="m1", text="", timestamp=2),
    ])
    write("10-visible-subset", [
        message("m2", "MODEL", parent="m1", text="answer", timestamp=2),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=2),
    ])
    write("11-current-node-path", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="first", timestamp=2),
        message("m3", "MODEL", parent="m1", text="second", timestamp=3),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="first", timestamp=2),
        message("m3", "MODEL", parent="m1", text="second", timestamp=3),
    ])
    write("12-orphan-parent", [
        message("m1", "USER", text="question", parent="missing", timestamp=1),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=2),
    ], [
        message("m1", "USER", text="question", parent="missing", timestamp=1),
        message("m2", "MODEL", parent="m1", text="answer", timestamp=2),
    ])
    write("13-error-participant", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "ERROR", parent="m1", text="failed", status="ERROR", timestamp=2),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "ERROR", parent="m1", text="failed", status="ERROR", timestamp=2),
    ])
    write("14-stopped-assistant", [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="partial", status="STOPPED", timestamp=2),
    ], [
        message("m1", "USER", text="question", timestamp=1),
        message("m2", "MODEL", parent="m1", text="partial", status="STOPPED", timestamp=2),
    ])
    write("15-empty-visible", [], [message("m1", "USER", text="hi")])
    write("16-many-branches", [
        message("m1", "USER", text="question", timestamp=1),
        *[message(f"m{index}", "MODEL", parent="m1", text=f"answer {index}", timestamp=index + 1)
          for index in range(2, 6)],
    ], [
        message("m1", "USER", text="question", timestamp=1),
        *[message(f"m{index}", "MODEL", parent="m1", text=f"answer {index}", timestamp=index + 1)
          for index in range(2, 6)],
    ])
    write("17-unicode", [
        message("m1", "USER", text="вопрос 🙂", timestamp=1),
        message("m2", "MODEL", parent="m1", text="ответ", timestamp=2),
    ], [
        message("m1", "USER", text="вопрос 🙂", timestamp=1),
        message("m2", "MODEL", parent="m1", text="ответ", timestamp=2),
    ])
    write("18-timestamps-equal", [
        message("m1", "USER", text="question", timestamp=5),
        message("m2", "MODEL", parent="m1", text="a", timestamp=5),
        message("m3", "MODEL", parent="m1", text="b", timestamp=5),
    ], [
        message("m1", "USER", text="question", timestamp=5),
        message("m2", "MODEL", parent="m1", text="a", timestamp=5),
        message("m3", "MODEL", parent="m1", text="b", timestamp=5),
    ])

    # ------------------------------------------------------------------- fuzz
    for index in range(8):
        rng = random.Random(15000 + index)
        messages = []
        for position in range(rng.randint(1, 8)):
            message_id = "m%d" % position
            parent = None
            if position > 0 and rng.random() < 0.8:
                parent = "m%d" % rng.randint(0, position - 1)
            messages.append(message(
                message_id,
                rng.choice(["USER", "MODEL", "MODEL", "ERROR"]),
                parent=parent,
                text=rng.choice(["", "text", "text 🙂"]),
                run=rng.choice([None, None, "run-1", "run-2"]),
                timestamp=rng.choice([0, 1, 2, 5]),
                status=rng.choice(["SUCCESS", "STOPPED", "ERROR", "THINKING"]),
            ))
        visible = [dict(entry) for entry in messages]
        if rng.random() < 0.4 and visible:
            visible = visible[rng.randint(0, len(visible) - 1):]
        if rng.random() < 0.3 and messages:
            messages = messages + [dict(messages[rng.randrange(len(messages))])]
        write("90-fuzz-%02d" % index, visible, messages)

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
