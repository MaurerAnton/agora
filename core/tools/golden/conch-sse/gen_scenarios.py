#!/usr/bin/env python3
"""Generates the scenario corpus for the conch-sse golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.

A scenario is {name, encrypted, lines, decrypt_error?} where `lines` are the raw SSE lines handed
to the parser. Encrypted scenarios get _conch_event / _conch_sequence injected into every frame,
because that is what the app produces over the wire.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

# kotlin SHELL_COMMAND_OUTPUT_MAX_BYTES (1 shl 20). The two truncation scenarios sit just above it.
BUDGET = 1 << 20

ALL = []


def add(name, lines, encrypted=False, decrypt_error=None):
    out = {"name": name, "encrypted": encrypted, "lines": lines}
    if decrypt_error is not None:
        out["decrypt_error"] = decrypt_error
    ALL.append(out)


def frames(encrypted, *pairs):
    """Builds SSE lines from (event, data) pairs, injecting the authentication fields if encrypted."""
    out = []
    sequence = 0
    for event, data in pairs:
        payload = dict(data)
        if encrypted:
            payload["_conch_event"] = event
            payload["_conch_sequence"] = sequence
            sequence += 1
        out.append(f"event: {event}")
        out.append("data: " + json.dumps(payload, ensure_ascii=False))
    return out


# ---- well-formed streams ---------------------------------------------------------------------------
add("single_line_then_result",
    frames(False, ("line", {"line": "hello", "stream": "stdout"}),
           ("result", {"exit_code": 0})))
add("stdout_and_stderr_interleave",
    frames(False, ("line", {"line": "out", "stream": "stdout"}),
           ("line", {"line": "err", "stream": "stderr"}),
           ("result", {"exit_code": 3})))
add("blank_line_between_frames_is_ignored",
    ["event: line", 'data: {"line": "hi", "stream": "stdout"}', "",
     "event: result", 'data: {"exit_code": 0}'])
add("empty_line_text_is_allowed",
    frames(False, ("line", {"line": "", "stream": "stdout"}),
           ("result", {"exit_code": 0})))
add("embedded_newlines_are_preserved",
    frames(False, ("line", {"line": "one\ntwo", "stream": "stdout"}),
           ("result", {"exit_code": 0})))
add("unknown_fields_are_ignored",
    ["event: line", 'data: {"line": "hi", "stream": "stdout", "future_key": [1, 2]}',
     "event: result", 'data: {"exit_code": 0, "elapsed_ms": 12}'])
add("warning_is_recorded",
    frames(False, ("warning", {"message": "careful"}), ("result", {"exit_code": 0})))
add("multiple_warnings_are_joined",
    frames(False, ("warning", {"message": "first"}), ("warning", {"message": "second"}),
           ("result", {"exit_code": 0})))
add("error_event_times_out",
    frames(False, ("error", {"message": "killed", "timed_out": True})))
add("error_event_does_not_time_out",
    frames(False, ("error", {"message": "refused", "timed_out": False})))
add("error_event_without_timeout_field",
    frames(False, ("error", {"message": "refused"})))
add("no_terminal_frame_keeps_the_streamed_output",
    frames(False, ("line", {"line": "partial", "stream": "stdout"})))

# ---- encrypted authentication --------------------------------------------------------------
add("encrypted_handshake_and_sequence",
    frames(True, ("line", {"line": "secure", "stream": "stdout"}),
           ("result", {"exit_code": 0})),
    encrypted=True)
add("encrypted_sequence_mismatch",
    # First frame claims sequence 1 while the parser expects 0.
    ["event: line",
     'data: {"line": "x", "stream": "stdout", "_conch_event": "line", "_conch_sequence": 1}'],
    encrypted=True)
add("encrypted_sequence_repeated",
    ["event: line",
     'data: {"line": "x", "stream": "stdout", "_conch_event": "line", "_conch_sequence": 0}',
     "event: result",
     'data: {"exit_code": 0, "_conch_event": "result", "_conch_sequence": 0}'],
    encrypted=True)
add("encrypted_sequence_is_a_string",
    ["event: line",
     'data: {"line": "x", "stream": "stdout", "_conch_event": "line", "_conch_sequence": "0"}'],
    encrypted=True)
add("encrypted_missing_event_field",
    ['event: line', 'data: {"line": "x", "stream": "stdout", "_conch_sequence": 0}'],
    encrypted=True)
add("encrypted_event_field_mismatch",
    ["event: line",
     'data: {"line": "x", "stream": "stdout", "_conch_event": "result", "_conch_sequence": 0}'],
    encrypted=True)
add("decrypt_failure_fails_closed",
    ["event: line", 'data: {"line": "x", "stream": "stdout", "_conch_event": "line", "_conch_sequence": 0}'],
    encrypted=True, decrypt_error="bad padding")

# ---- malformed framing ---------------------------------------------------------------------------
add("event_without_data_frame",
    ["event: line", "event: result", 'data: {"exit_code": 0}'])
add("empty_line_with_pending_event",
    ["event: line", ""])
add("event_after_terminal_frame",
    frames(False, ("result", {"exit_code": 0})) + ["event: line"])
add("data_after_terminal_frame",
    frames(False, ("result", {"exit_code": 0})) + ["data: {}"])
add("data_without_event",
    ['data: {"line": "x", "stream": "stdout"}'])
add("unsupported_event",
    ["event: meta", "{}"])
add("unexpected_line",
    ["this is not sse"])
add("event_without_space_is_unexpected",
    ["event:line", 'data: {}'])
add("stream_ends_with_pending_event",
    ["event: result"])

# ---- malformed payloads ---------------------------------------------------------------------------
add("invalid_json_payload",
    ["event: line", "data: {oops"])
add("line_event_missing_text",
    ["event: line", 'data: {"stream": "stdout"}'])
add("line_event_missing_stream",
    ["event: line", 'data: {"line": "x"}'])
add("line_event_blank_stream",
    ["event: line", 'data: {"line": "x", "stream": ""}'])
add("line_event_unsupported_stream",
    ["event: line", 'data: {"line": "x", "stream": "tty"}'])
add("result_event_missing_exit_code",
    ["event: result", "data: {}"])
add("result_event_exit_code_is_a_string",
    ["event: result", 'data: {"exit_code": "0"}'])
add("warning_event_missing_message",
    ["event: warning", "data: {}"])
add("error_event_missing_message",
    ["event: error", 'data: {"timed_out": true}'])
add("error_event_timeout_is_a_string",
    ["event: error", 'data: {"message": "slow", "timed_out": "true"}'])
add("error_event_timeout_is_not_a_boolean",
    ["event: error", 'data: {"message": "slow", "timed_out": "yes"}'])

# ---- truncation: both the retained and the emitted output share the byte budget --------------
def long_line(text):
    return frames(False, ("line", {"line": text, "stream": "stdout"}),
                  ("line", {"line": "tail", "stream": "stdout"}),
                  ("result", {"exit_code": 0}))


add("ascii_output_is_truncated_at_the_byte_budget",
    long_line("a" * (BUDGET + 64)))
# "€" is three UTF-8 bytes; with two bytes of budget left it must not be split, so the retained
# prefix stops one character earlier than the raw budget would suggest.
add("multibyte_character_is_never_split",
    long_line("a" * (BUDGET - 2) + "€"))


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
