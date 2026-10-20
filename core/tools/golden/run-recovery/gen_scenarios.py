#!/usr/bin/env python3
"""Generates the run-recovery scenarios. Deterministic; rerun after editing.

Every branch of RunRecoveryPolicy.kt is covered:
  * the full Participant x MessageStatus matrix (21 pairs),
  * every terminal tool state plus calling, running, missing, unknown and case variants,
  * every message-segment field through the Kotlin data-class toString(),
  * the raw-JSON path: unchanged-vs-rewritten, non-array and malformed roots, non-object
    elements, absent/null/number/bool/array/object states, terminal states, key order,
    duplicate keys, compact formatting, Unicode and escapes.

The raw strings are literals so malformed input stays exactly as written; json.dump only escapes
them into the scenario file.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []

MISSING = object()

STATUSES = [
    "TRANSCRIBING",
    "SENDING",
    "THINKING",
    "TOOL_CALLING",
    "SUCCESS",
    "STOPPED",
    "ERROR",
]

TERMINAL = ["succeeded", "empty", "failed", "stopped", "background_running"]


def case(name, fn, **fields):
    ALL.append({"name": name, "fn": fn, **fields})


def status_case(participant):
    case(f"status_{participant.lower()}_all", "recoverMessageStatus",
         participant=participant, statuses=STATUSES)


# ---------------------------------------------------------------------------------------------
# recoverMessageStatus: every participant and every status
# ---------------------------------------------------------------------------------------------

status_case("USER")
status_case("MODEL")
status_case("ERROR")

# ---------------------------------------------------------------------------------------------
# stopIncompleteTools: MessageSegment list in, list out
# ---------------------------------------------------------------------------------------------


def segment(type_="tool", state=MISSING, **overrides):
    entry = {"type": type_, "content": "segment-content"}
    if state is not MISSING:
        entry["toolState"] = state
    entry.update(overrides)
    return entry


def full_segment(**overrides):
    """A MessageSegment with every constructor field populated, including both @Transient ones."""
    entry = {
        "type": "tool",
        "content": "answer | with ; separators\nsecond line\tand tab",
        "toolName": "shell",
        "toolArgs": "{\"command\":\"echo hi\"}",
        "toolResult": "ok",
        "toolCallId": "call-1",
        "signature": "c2lnbmF0dXJl",
        "signatureProvider": "anthropic",
        "durationMs": 1234,
        "toolState": "running",
        "toolProgress": "50%",
        "toolTarget": "device",
        "toolDisplayName": "Shell command",
        "toolResultText": "done",
        "toolStructuredResult": "{\"ok\":true}",
        "toolImages": [
            {
                "path": "/data/a.png",
                "mimeType": "image/png",
                "sizeBytes": 7,
                "width": 2,
                "height": 3,
                "sha256": "aa",
            },
            {
                "path": "/data/b.png",
                "mimeType": "image/jpeg",
                "sizeBytes": 11,
                "width": None,
                "height": 9,
                "sha256": "bb",
            },
        ],
        "toolImageRequestKey": "request-key",
        "toolTranscription": "a screenshot",
        "responseOutputItems": [
            {"type": "text", "text": "hi"},
            {"nested": {"b": [1, True, None], "a": "é"}},
        ],
        "responseOutputItemProvider": "openai",
        "errorCode": "E42",
        "streamingTextDeltas": [
            {"sequence": 1, "codePointCount": 2},
            {"sequence": 2, "codePointCount": 3},
        ],
    }
    entry.update(overrides)
    return entry


case("segments_empty", "stopIncompleteTools", segments=[])

case(
    "segments_terminal_states_are_untouched",
    "stopIncompleteTools",
    segments=[segment(state=state, toolName=state) for state in TERMINAL],
)

case(
    "segments_non_terminal_states_are_stopped",
    "stopIncompleteTools",
    segments=[
        segment(state="calling", toolName="one"),
        segment(state="running", toolName="two"),
        segment(state=MISSING, toolName="three"),
    ],
)

case(
    "segments_unknown_states_are_stopped",
    "stopIncompleteTools",
    segments=[
        segment(state="paused"),
        segment(state=""),
        segment(state="STOPPED"),
        segment(state="background-running"),
        segment(state=" stopped"),
    ],
)

case(
    "segments_non_tool_segments_are_untouched",
    "stopIncompleteTools",
    segments=[
        segment(type_="answer", state=MISSING),
        segment(type_="thought", state="calling"),
        segment(type_="error", state="running"),
        segment(type_="citation", state=MISSING),
        segment(type_="Tool", state="calling"),
    ],
)

case(
    "segments_preserves_every_untouched_field",
    "stopIncompleteTools",
    segments=[full_segment()],
)

case(
    "segments_mixed_order",
    "stopIncompleteTools",
    segments=[
        segment(type_="answer", state=MISSING),
        full_segment(toolState="calling"),
        segment(state="succeeded", toolName="finished"),
        segment(state=MISSING, toolName="missing"),
        segment(type_="thought", state="running"),
    ],
)

# ---------------------------------------------------------------------------------------------
# stopIncompleteToolsJson
# ---------------------------------------------------------------------------------------------

# Unchanged: no tool segment needs a rewrite, so the raw text must come back byte for byte.
case(
    "json_unchanged_non_tool_returns_raw_verbatim",
    "stopIncompleteToolsJson",
    raw='[\n  {"type": "answer", "toolState": "calling"},\n'
        '  {"type":"thought","toolState":null},\n  42, "x", null, [1,2]\n]\n',
)
case(
    "json_unchanged_terminal_states_return_raw_verbatim",
    "stopIncompleteToolsJson",
    raw='[\n  {"type": "tool", "toolState": "succeeded"},\n'
        '  {"toolState":"empty","type":"tool"},\n'
        '  {"type":"tool","toolState":"failed"},\n'
        '  {"type":"tool","toolState":"stopped"},\n'
        '  {"type":"tool","toolState":"background_running"}\n]  ',
)
case(
    "json_empty_array_returns_raw_verbatim",
    "stopIncompleteToolsJson",
    raw=" [ ] \n",
)

# Changed: missing state appended at the end.
case(
    "json_changed_missing_state_is_appended_at_end",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolName":"shell","toolArgs":"{}","content":"a"}]',
)
case(
    "json_changed_existing_state_is_replaced_in_place",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"calling","toolName":"shell"}]',
)
case(
    "json_changed_compacts_source_whitespace",
    "stopIncompleteToolsJson",
    raw='[\n\t{ "type" : "tool", "content" : "x" },\n\t{ "type" : "answer" }\n]\n',
)
case(
    "json_changed_mixed_elements_keep_order_and_shape",
    "stopIncompleteToolsJson",
    raw='[null,{"type":"answer","toolState":"calling"},{"type":"tool","content":"a"},'
        '{"type":"tool","toolState":"succeeded"},[1,2],"x",'
        '{"type":"tool","toolState":"running"}]',
)
case(
    "json_changed_preserves_nested_key_order",
    "stopIncompleteToolsJson",
    raw='[{"z":1,"type":"tool","a":{"b":2,"a":1},"m":[{"y":2,"x":1},true,null],'
        '"content":"x"}]',
)
case(
    "json_changed_unicode_and_escapes_are_recoded",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","content":"caf\\u00e9 \\u65e5\\u672c\\u8a9e '
        '\\ud83d\\ude00 \\/ slash \\\\ quote \\" tab \\t newline \\n ctrl \\u0001",'
        '"na\\u006de":"value"}]',
)
case(
    "json_nested_tool_objects_are_not_rewritten",
    "stopIncompleteToolsJson",
    raw='[{"type":"answer","nested":{"type":"tool"}},'
        '{"type":"tool","toolState":"succeeded","nested":[{"type":"tool"}]}]',
)

# State shapes: every non-terminal, missing and non-string state is rewritten.
case(
    "json_state_null_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":null}]',
)
case(
    "json_state_number_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":5}]',
)
case(
    "json_state_bool_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":true}]',
)
case(
    "json_state_array_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":[]}]',
)
case(
    "json_state_object_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":{}}]',
)
case(
    "json_state_empty_string_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":""}]',
)
case(
    "json_state_unknown_string_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"paused"}]',
)
case(
    "json_state_uppercase_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"STOPPED"}]',
)
case(
    "json_state_whitespace_is_stopped",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":" stopped "}]',
)

# Type shapes: only the primitive content "tool" is rewritten.
case(
    "json_type_null_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":null,"toolState":"calling"}]',
)
case(
    "json_type_number_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":5,"toolState":"calling"}]',
)
case(
    "json_type_bool_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":true,"toolState":"calling"}]',
)
case(
    "json_type_array_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":["tool"],"toolState":"calling"}]',
)
case(
    "json_type_object_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":{"value":"tool"},"toolState":"calling"}]',
)
case(
    "json_type_uppercase_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"type":"Tool","toolState":"calling"}]',
)
case(
    "json_type_missing_is_not_tool",
    "stopIncompleteToolsJson",
    raw='[{"toolState":"calling"}]',
)

# Duplicate keys: kotlinx keeps the last value at the first key position.
case(
    "json_duplicate_state_last_non_terminal_wins",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"succeeded","toolState":"calling"}]',
)
case(
    "json_duplicate_state_last_terminal_returns_raw",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"calling","toolState":"succeeded"}]',
)
case(
    "json_duplicate_type_last_non_tool_returns_raw",
    "stopIncompleteToolsJson",
    raw='[{"type":"tool","toolState":"calling","type":"answer"}]',
)
case(
    "json_duplicate_type_last_tool_is_rewritten",
    "stopIncompleteToolsJson",
    raw='[{"type":"answer","toolState":"calling","type":"tool"}]',
)

# Roots that are not arrays.
case("json_root_object", "stopIncompleteToolsJson", raw='{"type":"tool"}')
case("json_root_string", "stopIncompleteToolsJson", raw='"tool"')
case("json_root_number", "stopIncompleteToolsJson", raw="42")
case("json_root_null", "stopIncompleteToolsJson", raw="null")
case("json_root_bool", "stopIncompleteToolsJson", raw="true")

# Malformed documents: kotlinx throws, so the Kotlin function returns null.
case("json_malformed_empty", "stopIncompleteToolsJson", raw="")
case("json_malformed_blank", "stopIncompleteToolsJson", raw=" \n\t ")
case("json_malformed_unclosed", "stopIncompleteToolsJson", raw='[{"type":"tool"')
case("json_malformed_trailing_comma", "stopIncompleteToolsJson", raw='[{"type":"tool"},]')
case("json_malformed_trailing_content", "stopIncompleteToolsJson", raw="[]x")
case("json_malformed_invalid_escape", "stopIncompleteToolsJson", raw='[{"type":"\\q"}]')
case("json_malformed_bom", "stopIncompleteToolsJson", raw='\ufeff[{"type":"tool"}]')
case("json_malformed_single_quotes", "stopIncompleteToolsJson", raw="[{'type':'tool'}]")
case("json_malformed_missing_colon", "stopIncompleteToolsJson", raw='[{"type" "tool"}]')


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
