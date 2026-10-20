#!/usr/bin/env python3
"""Generates Migration17To18 scenarios. Deterministic; rerun after editing.

Each scenario supplies the conversations/runs/messages projections the recording database
returns. The frozen trace pins every query and execSQL the migration performs (with typed bind
arguments) in planning order, plus any exception class/message.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, conversations, runs, messages):
    ALL.append(
        {
            "name": name,
            "op": "migrate",
            "conversations": conversations,
            "runs": runs,
            "messages": messages,
        }
    )


def conversation(cid, selections=None, run_selections=None):
    fields = {"id": cid}
    if selections is not None:
        fields["selectedBranchesJson"] = selections
    if run_selections is not None:
        fields["selectedRunBranchesJson"] = run_selections
    return fields


def run(cid, rid, parent, started):
    return {
        "conversationId": cid,
        "id": rid,
        "parentRunId": parent,
        "startedAt": started,
    }


def message(cid, mid, parent, participant, timestamp, rid, sequence, text="hello",
            images="", attachment=None):
    fields = {
        "conversationId": cid,
        "id": mid,
        "parentId": parent,
        "text": text,
        "images": images,
        "participant": participant,
        "timestamp": timestamp,
        "runId": rid,
        "runSequence": sequence,
    }
    if attachment is not None:
        fields["attachmentMeta"] = attachment
    return fields


def repair_fields(mid, parent, participant, timestamp, rid, sequence, text, images, attachment):
    return message("c1", mid, parent, participant, timestamp, rid, sequence, text, images, attachment)


def repair_case(name, selections=None, run_selections=None, text1="hello", text2="hello",
                images1="", images2="", attachment1=None, attachment2=None,
                started1=100, started2=200, participant2="USER"):
    case(
        name,
        [conversation("c1", selections, run_selections)],
        [run("c1", "r1", None, started1), run("c1", "r2", None, started2)],
        [
            message("c1", "u1", "p0", "USER", 10, "r1", 0, text1, images1, attachment1),
            message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
            message("c1", "u2", "p0", participant2, 12, "r2", 0, text2, images2, attachment2),
            message("c1", "m2", "u2", "MODEL", 13, "r2", 1),
        ],
    )


case("m17_empty", [], [], [])
case(
    "m17_no_repair",
    [conversation("c1")],
    [run("c1", "r1", None, 100)],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
    ],
)
repair_case("m17_repair_pair")
repair_case(
    "m17_repair_selected_boundary",
    selections=json.dumps({"p0": "u2"}, separators=(",", ":")),
)
repair_case(
    "m17_repair_selected_run",
    run_selections=json.dumps({"null": "r2"}, separators=(",", ":")),
)
repair_case(
    "m17_repair_anchor_selected_stale",
    selections=json.dumps({"p0": "u1"}, separators=(",", ":")),
)
repair_case("m17_different_text", text2="hello!")
repair_case(
    "m17_attachment_uri_ignored",
    attachment1='{"originalUri":"file:///a","mime":"image/png"}',
    attachment2='{"originalUri":"file:///b","mime":"image/png"}',
)
repair_case(
    "m17_attachment_differs",
    attachment1='{"originalUri":"file:///a","mime":"image/png"}',
    attachment2='{"originalUri":"file:///b","mime":"image/jpeg"}',
)
repair_case("m17_images_json", images1='["a","b"]', images2='["a","b"]')
repair_case("m17_images_fallback", images1="a|||b", images2="a|||b")
repair_case("m17_images_paths_agree", images1='["a","b"]', images2="a|||b")
repair_case("m17_images_empty_vs_empty_array", images1="", images2="[]")
repair_case("m17_images_differ", images1="[]", images2='["a"]')
repair_case(
    "m17_invalid_selections",
    selections="{bad",
    run_selections='{"a":1}',
)
case(
    "m17_two_conversations",
    [
        conversation("c2", json.dumps({"p0": "u2"}, separators=(",", ":"))),
        conversation("c1"),
    ],
    [
        run("c2", "r1", None, 100),
        run("c2", "r2", None, 200),
        run("c1", "r3", None, 300),
    ],
    [
        message("c2", "u1", "p0", "USER", 10, "r1", 0),
        message("c2", "m1", "u1", "MODEL", 11, "r1", 1),
        message("c2", "u2", "p0", "USER", 12, "r2", 0),
        message("c2", "m2", "u2", "MODEL", 13, "r2", 1),
        message("c1", "u3", "p0", "USER", 20, "r3", 0),
        message("c1", "m3", "u3", "MODEL", 21, "r3", 1),
    ],
)
repair_case("m17_unknown_participant", participant2="BOGUS")
case(
    "m17_three_runs",
    [conversation("c1")],
    [
        run("c1", "r1", None, 100),
        run("c1", "r2", None, 200),
        run("c1", "r3", None, 300),
    ],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
        message("c1", "u2", "p0", "USER", 12, "r2", 0),
        message("c1", "m2", "u2", "MODEL", 13, "r2", 1),
        message("c1", "u3", "p0", "USER", 14, "r3", 0),
        message("c1", "m3", "u3", "MODEL", 15, "r3", 1),
    ],
)
case(
    "m17_started_tie",
    [conversation("c1")],
    [run("c1", "r2", None, 100), run("c1", "r1", None, 100)],
    [
        message("c1", "u2", "p0", "USER", 12, "r2", 0),
        message("c1", "m2", "u2", "MODEL", 13, "r2", 1),
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
    ],
)
case(
    "m17_synthetic_boundary",
    [conversation("c1")],
    [run("c1", "r1", None, 100), run("c1", "r2", None, 200)],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
        message("c1", "tool_u2", "p0", "USER", 12, "r2", 0),
        message("c1", "m2", "tool_u2", "MODEL", 13, "r2", 1),
    ],
)
case(
    "m17_no_inferred_with_selections",
    [conversation("c1", json.dumps({"p0": "u1"}, separators=(",", ":")))],
    [run("c1", "r1", None, 100)],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
    ],
)
repair_case(
    "m17_selection_to_retained",
    selections=json.dumps({"p0": "m2"}, separators=(",", ":")),
)
case(
    "m17_no_root_output",
    [conversation("c1")],
    [run("c1", "r1", None, 100), run("c1", "r2", None, 200)],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
        message("c1", "u2", "p0", "USER", 12, "r2", 0),
    ],
)
case(
    "m17_no_leading_inputs",
    [conversation("c1")],
    [run("c1", "r1", None, 100), run("c1", "r2", None, 200)],
    [
        message("c1", "u1", "p0", "USER", 10, "r1", 0),
        message("c1", "m1", "u1", "MODEL", 11, "r1", 1),
        message("c1", "u2", "p0", "USER", 12, "r2", 0),
        message("c1", "m2", "u2", "MODEL", 13, "r2", 0),
    ],
)


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for existing in os.listdir(SCENARIOS_DIR):
        if existing.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, existing))
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")


if __name__ == "__main__":
    main()
