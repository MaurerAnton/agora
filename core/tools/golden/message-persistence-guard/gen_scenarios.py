#!/usr/bin/env python3
"""Generates deterministic scenarios for MessagePersistenceGuard."""
import json
import os
import random

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")
CASES = []


def case(name, fn="encodeSegmentsBounded", **fields):
    CASES.append({"name": name, "fn": fn, **fields})


def segment(**fields):
    return fields


def image(path="media/a.png", mime="image/png", size=1234, **extra):
    value = {"path": path, "mimeType": mime, "sizeBytes": size, "sha256": "ab" * 32}
    value.update(extra)
    return value


# Empty / ordinary paths.
case("null_segments", segments_json=None)
case("empty_segments", segments_json=[])
case("canonical_empty_answer", segments_json=[segment(type="answer")])
case(
    "canonical_rich_segment",
    segments_json=[
        segment(
            type="answer",
            content="quoted \" text \\ newline\n control\u0001 😀",
            signature="sig",
            signatureProvider="provider",
        ),
        segment(
            type="tool",
            content="tool content",
            toolName="lookup",
            toolArgs="{\"query\":\"value\"}",
            toolResult="result",
            toolResultText="display",
            toolStructuredResult="{\"ok\":true}",
            toolProgress="complete",
            toolCallId="call-1",
            durationMs=1234,
            toolState="succeeded",
            toolTarget="target",
            toolDisplayName="Lookup",
            toolTranscription="transcription",
            responseOutputItems=[{"id": "m1", "type": "message"}],
            responseOutputItemProvider="OpenAI",
            errorCode="none",
            toolImages=[image(width=10, height=20)],
        ),
    ],
)
case(
    "canonical_empty_fields_are_preserved",
    segments_json=[
        segment(
            type="tool",
            toolResult="",
            toolProgress="",
            toolResultText="",
            toolStructuredResult="",
        )
    ],
)
case(
    "canonical_empty_response_items_are_omitted",
    segments_json=[segment(type="tool", responseOutputItems=[])],
)

# An oversized payload that is trimmed before encoding.
case(
    "default_budget_is_600000",
    segments_json=[segment(type="tool", toolResult="d" * 620000)],
)
case(
    "oversized_payload_is_trimmed",
    maxBytes=4000,
    segments_json=[segment(type="tool", toolResult="r" * 8000)],
)
case(
    "two_segments_trim_the_largest_first",
    maxBytes=4000,
    segments_json=[
        segment(type="tool", toolName="first", toolResult="r" * 3000),
        segment(type="tool", toolName="second", toolResultText="t" * 5000),
    ],
)
case(
    "tie_across_segments_picks_first",
    maxBytes=5500,
    segments_json=[
        segment(type="tool", toolName="first", toolResult="a" * 3000),
        segment(type="tool", toolName="second", toolResult="b" * 3000),
    ],
)
case(
    "tie_prefers_tool_result_then_result_text",
    maxBytes=10000,
    segments_json=[
        segment(
            type="tool",
            toolResult="r" * 3000,
            toolResultText="t" * 3000,
            toolStructuredResult="s" * 3000,
            toolProgress="p" * 3000,
        )
    ],
)
case(
    "tie_single_trim_prefers_tool_result",
    maxBytes=11000,
    segments_json=[
        segment(
            type="tool",
            toolResult="r" * 3000,
            toolResultText="t" * 3000,
            toolStructuredResult="s" * 3000,
            toolProgress="p" * 3000,
        )
    ],
)
case(
    "answer_content_is_trimmed",
    maxBytes=3000,
    segments_json=[segment(type="answer", content="c" * 5000)],
)
case(
    "tool_content_is_never_counted_or_trimmed",
    maxBytes=2000,
    segments_json=[segment(type="tool", content="c" * 5000)],
)
case(
    "payload_fits_but_metadata_exceeds",
    maxBytes=4000,
    segments_json=[
        segment(type="answer", content="c" * 3000, toolArgs="a" * 2000),
    ],
)
case(
    "floor_exactly_is_not_trimmable",
    maxBytes=1000,
    segments_json=[segment(type="tool", toolResult="x" * 2000)],
)
case(
    "floor_plus_one_is_trimmed",
    maxBytes=2033,
    segments_json=[segment(type="tool", toolResult="x" * 2001)],
)
case(
    "trim_json_escapes_at_floor_boundary",
    maxBytes=2035,
    segments_json=[
        segment(type="tool", toolResult="x" * 1968 + '"' * 2 + "x" * 31),
    ],
)
case(
    "structured_result_is_trimmed",
    maxBytes=3000,
    segments_json=[segment(type="tool", toolStructuredResult="s" * 5000)],
)
case(
    "progress_is_trimmed",
    maxBytes=3000,
    segments_json=[segment(type="tool", toolProgress="p" * 5000)],
)
case(
    "result_text_is_trimmed",
    maxBytes=3000,
    segments_json=[segment(type="tool", toolResultText="t" * 5000)],
)
case(
    "multi_field_trimming_terminates",
    maxBytes=5000,
    segments_json=[
        segment(
            type="tool",
            toolResult="r" * 3000,
            toolResultText="t" * 4000,
            toolStructuredResult="s" * 5000,
            toolProgress="p" * 6000,
        )
    ],
)
case(
    "tool_content_with_trimmable_result",
    maxBytes=7100,
    segments_json=[
        segment(type="tool", content="c" * 5000, toolResult="r" * 5000),
    ],
)
case(
    "empty_result_fields_stay_empty_while_progress_trims",
    maxBytes=3000,
    segments_json=[
        segment(
            type="tool",
            toolResult="",
            toolProgress="p" * 5000,
            toolResultText="",
            toolStructuredResult="",
        )
    ],
)
case(
    "empty_fields_placeholder_count_boundary",
    maxBytes=3000,
    segments_json=[
        segment(
            type="tool",
            toolResult="",
            toolProgress="p" * 5000,
            toolResultText="",
            toolStructuredResult="",
            toolArgs="a" * 372,
        )
    ],
)
case(
    "empty_answer_content_is_not_a_placeholder",
    maxBytes=3000,
    segments_json=[segment(type="answer", content="", toolResult="r" * 5000)],
)
case(
    "responses_state_survives_trimming",
    maxBytes=3000,
    segments_json=[
        segment(
            type="tool",
            toolResult="r" * 5000,
            responseOutputItems=[{"type": "message", "id": "m1"}],
            responseOutputItemProvider="OpenAI",
        )
    ],
)
case(
    "answer_and_result_content_are_both_trimmed",
    maxBytes=4000,
    segments_json=[
        segment(type="answer", content="c" * 3000, toolResult="r" * 5000),
    ],
)
case(
    "trim_does_not_split_a_surrogate_pair",
    maxBytes=3000,
    segments_json=[
        segment(type="tool", toolResult="a" * 2500 + "😀" + "b" * 2500),
    ],
)
case(
    "trim_counts_json_escapes",
    maxBytes=2500,
    segments_json=[
        segment(type="tool", toolResult=("\"\\\n\t\u0001😀" * 500)),
    ],
)
case(
    "many_untrimmable_segments_fail_closed",
    maxBytes=8000,
    segments_json=[
        segment(type="tool", toolName=f"tool_{index}", toolArgs='{"payload":"' + "x" * 2000 + '"}')
        for index in range(40)
    ],
)
case(
    "zero_budget_fails_closed",
    maxBytes=0,
    segments_json=[segment(type="tool", toolResult="r" * 3000)],
)
case(
    "negative_budget_fails_closed",
    maxBytes=-5,
    segments_json=[segment(type="tool", toolResult="r" * 3000)],
)

# Protected Responses continuation state.
case(
    "responses_state_oversized_throws",
    maxBytes=512,
    segments_json=[
        segment(
            type="tool",
            toolName="lookup",
            responseOutputItems=[
                {"id": "rs_1", "type": "reasoning", "encrypted_content": "x" * 4000}
            ],
            responseOutputItemProvider="OpenAI",
        )
    ],
)
case(
    "responses_state_that_fits_is_kept",
    segments_json=[
        segment(
            type="tool",
            toolName="lookup",
            toolResult="ok",
            responseOutputItems=[
                {"type": "message", "id": "m1", "content": [{"type": "output_text", "text": "ok"}]}
            ],
            responseOutputItemProvider="OpenAI",
        )
    ],
)

# clipText, with exact UTF-16 boundaries.
case("clip_text_exact_limit", fn="clipText", text="a" * 100000)
case("clip_text_over_limit", fn="clipText", text="a" * 100001)
case(
    "clip_text_stops_before_split_surrogate",
    fn="clipText",
    text="a" * 99999 + "😀" + "tail",
)
case(
    "clip_text_keeps_astral_ending_before_boundary",
    fn="clipText",
    text="a" * 99998 + "😀" + "tail",
)
case(
    "clip_text_exact_limit_ending_in_emoji",
    fn="clipText",
    text="a" * 99998 + "😀",
)
case("clip_text_empty", fn="clipText", text="")


# Deterministic differential corpus: random mixes of trimmable fields, Unicode and escapes at
# budgets that force zero, one and several trim rounds. These are ordinary frozen traces, not a
# separate randomized test, so every failure is reproducible.
_rng = random.Random(20261020)
_alphabet = [
    "a", "b", "x", " ", "\n", "\t", "\u0001", '"', "\\", "😀", "漢", "é",
    "\u2028", "𝄞", "e\u0301",
]


def _text(size):
    return "".join(_rng.choice(_alphabet) for _ in range(size))


for _index in range(24):
    _fields = {}
    _kind = _rng.choice(["tool", "tool", "tool", "answer", "thought", "citation"])
    _fields["type"] = _kind
    if _kind == "tool":
        if _rng.random() < 0.85:
            _fields["content"] = _text(_rng.choice([0, 10, 2000, 4000]))
        if _rng.random() < 0.9:
            _fields["toolResult"] = _text(_rng.choice([0, 5, 1999, 2001, 3000, 5000]))
        if _rng.random() < 0.6:
            _fields["toolResultText"] = _text(_rng.choice([0, 2000, 2500, 4000]))
        if _rng.random() < 0.6:
            _fields["toolStructuredResult"] = _text(_rng.choice([0, 2001, 3500]))
        if _rng.random() < 0.6:
            _fields["toolProgress"] = _text(_rng.choice([0, 2000, 4000]))
        if _rng.random() < 0.5:
            _fields["toolArgs"] = _text(_rng.choice([10, 500, 1500]))
        if _rng.random() < 0.3:
            _fields["responseOutputItems"] = [{"type": "message", "id": f"m{_index}"}]
    else:
        _fields["content"] = _text(_rng.choice([0, 100, 2000, 2001, 3000, 5000]))
        if _rng.random() < 0.4:
            _fields["toolResult"] = _text(_rng.choice([0, 2500, 4000]))
    _budget = _rng.choice([0, 100, 500, 1200, 2000, 3000, 4000, 5000, 8000])
    case(f"fuzz_{_index:02d}", maxBytes=_budget, segments_json=[segment(**_fields)])


def main():
    os.makedirs(SCENARIOS, exist_ok=True)
    for old in os.listdir(SCENARIOS):
        if old.endswith(".json"):
            os.remove(os.path.join(SCENARIOS, old))
    for entry in CASES:
        with open(os.path.join(SCENARIOS, entry["name"] + ".json"), "w", encoding="utf-8") as handle:
            json.dump(entry, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"wrote {len(CASES)} scenarios to {SCENARIOS}")


if __name__ == "__main__":
    main()
