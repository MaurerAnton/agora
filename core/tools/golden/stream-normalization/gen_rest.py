# Accumulator, close-boundary, termination, malformed, carries-output, retry scenarios.
# Run from this directory: python3 gen_rest.py
from genlib import THINK_CLOSE, ev, w

# ---- ToolArgumentAccumulator ----
w("accum_snapshot_growing", {"mode": "accumulator", "fragments": ['{"a":1', '}', '{"a":1}', '{"a":12}']})

w("accum_stale_snapshot", {"mode": "accumulator", "fragments": [
    '{"key_aaaaaaaaaaaaaaaaaaaaaa":1}', '{"key_aaaaaaaaaaaaaaaaaaaaaa":1}'
]})

w("accum_short_head_kept", {"mode": "accumulator", "fragments": ['[', '{"a":1}']})

w("accum_empty_never_erases", {"mode": "accumulator", "fragments": ['{"a":1}', "", None]})

# ---- findImplicitThinkingClose ----
w("closeb_simple", {"mode": "closeBoundary", "text": "reasoning then " + THINK_CLOSE + " done"})

w("closeb_fence_skipped", {"mode": "closeBoundary", "text":
    "```\n" + THINK_CLOSE + "\n```\nthen " + THINK_CLOSE + " end"})

w("closeb_inline_skipped", {"mode": "closeBoundary", "text": "`" + THINK_CLOSE + "` real "})

w("closeb_indented_skipped", {"mode": "closeBoundary", "text": "   " + THINK_CLOSE + "\ntext "})

w("closeb_none", {"mode": "closeBoundary", "text": "no markers here"})

w("closeb_channel_marker", {"mode": "closeBoundary", "text":
    "reasoning <|channel|>final<|message|> tail"})

# ---- StreamTermination ----
w("term_clean_finish", {"mode": "termination", "sawTerminalMarker": True, "stopReason": "stop"})

w("term_token_cap", {"mode": "termination", "sawTerminalMarker": True, "stopReason": "length"})

w("term_retry_no_terminal", {"mode": "termination", "sawTerminalMarker": False})

w("term_retry_after_content", {"mode": "termination", "sawTerminalMarker": False, "producedContent": True})

w("term_tool_in_flight", {"mode": "termination", "sawTerminalMarker": True, "toolCallInFlight": True})

w("term_stream_error_retryable", {"mode": "termination", "hasStreamError": True,
                                  "streamErrorMessage": "The server response could not be read"})

w("term_stream_error_fatal", {"mode": "termination", "hasStreamError": True,
                              "streamErrorMessage": "invalid api key", "retryableStreamError": False})

w("term_timeout", {"mode": "termination", "sawTerminalMarker": False, "timedOut": True})

w("term_reported_error", {"mode": "termination", "sawTerminalMarker": False, "alreadyReportedError": True})

# ---- MalformedToolCall ----
w("malformed_full", {"mode": "malformed", "cause": "arguments were cut off",
                     "originalName": "calc", "originalArguments": '{"a":1}', "signature": "sig"})

w("malformed_bare", {"mode": "malformed", "cause": "unparsable payload"})

w("malformed_truncation", {"mode": "malformed", "cause": "truncated",
                           "originalName": "n" * 300, "originalArguments": "x" * 3000})

# ---- carriesModelOutput ----
w("carry_matrix", {"mode": "carriesOutput", "events": [ev("text", text="hi")]})
w("carry_empty_text", {"mode": "carriesOutput", "events": [ev("text", text="")]})
w("carry_thought", {"mode": "carriesOutput", "events": [ev("thought", thought="t")]})
w("carry_thought_signature", {"mode": "carriesOutput", "events": [ev("thought", signature="s")]})
w("carry_usage", {"mode": "carriesOutput", "events": [ev("usage", tokenCount=1)]})

# ---- ProviderRetryPolicy ----
w("retry_delays", {"mode": "retry", "retryNumber": 3})
w("retry_invalid", {"mode": "retry", "retryNumber": 0})

print("rest scenarios written")
