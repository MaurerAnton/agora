# Batch 2: closes the branch gaps found in review (termination guards, retry probes,
# parser edge inputs, disabled-thinking paths, carriesModelOutput matrix).
# Run from this directory: python3 gen_batch2.py
from genlib import INVOKE_OPEN, TOOL_CLOSE, TOOL_OPEN, ev, w

XML_LEADING = (
    INVOKE_OPEN + ' name="calc">' + "<" + 'parameter name="a">1<' + '/parameter>' + "<" + '/invoke>'
)

# ---- StreamTermination: token-cap spellings and guard ordering ----
w("term_token_cap_anthropic", {"mode": "termination", "sawTerminalMarker": True, "stopReason": "max_tokens"})
w("term_token_cap_gemini", {"mode": "termination", "sawTerminalMarker": True, "stopReason": "max_output_tokens"})
w("term_cap_overrides_no_terminal", {"mode": "termination", "sawTerminalMarker": False, "stopReason": "length"})

# ---- StreamTermination: stream-error retry decision isolated from the no-terminal branch ----
w("term_stream_error_fatal_terminal", {"mode": "termination", "sawTerminalMarker": True,
                                       "hasStreamError": True, "streamErrorMessage": "invalid api key",
                                       "retryableStreamError": False})
w("term_stream_error_retryable_terminal", {"mode": "termination", "sawTerminalMarker": True,
                                           "hasStreamError": True, "streamErrorMessage": "some relay failure",
                                           "retryableStreamError": True})
w("term_stream_error_policy_override", {"mode": "termination", "sawTerminalMarker": True,
                                        "hasStreamError": True,
                                        "streamErrorMessage": "The server response could not be read",
                                        "retryableStreamError": False})

# ---- toolParser: additional damaged/edge inputs ----
w("parsep_call_id_only", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"call_id":"call_c","name":"calc","arguments":"{}"}' + TOOL_CLOSE})
w("parsep_missing_arguments", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"name":"calc"}' + TOOL_CLOSE})
w("parsep_numeric_arguments", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"name":"calc","arguments":5}' + TOOL_CLOSE})

# ---- normalizer: XML probe leading the content ----
w("norm_xml_invoke_leading", {"mode": "normalizer", "tools": ["calc"], "events": [
    ev("text", text=XML_LEADING)
]})

# ---- carriesModelOutput: remaining event kinds ----
w("carry_tool_call", {"mode": "carriesOutput", "events": [
    ev("toolCall", id="call_a", name="calc", arguments="{}")]})
w("carry_tool_calls", {"mode": "carriesOutput", "events": [
    ev("toolCalls", calls=[ev("toolCall", id="call_a", name="calc", arguments="{}")])]})
w("carry_tool_call_update", {"mode": "carriesOutput", "events": [
    ev("toolCallUpdate", streamKey="k", name="calc")]})
w("carry_hosted", {"mode": "carriesOutput", "events": [
    ev("hosted", streamKey="k", name="web")]})
w("carry_retrying", {"mode": "carriesOutput", "events": [ev("retrying", attempt=1, maxAttempts=5)]})
w("carry_error", {"mode": "carriesOutput", "events": [ev("error")]})
w("carry_citation", {"mode": "carriesOutput", "events": [ev("citation")]})

# ---- thinkingParser: disabled-thinking and flush overloads ----
w("think_disabled_feed", {"mode": "thinkingParser", "thinkingEnabled": False, "chunks": [
    "<think", "ing>hidden", "<" + "/thinking>shown"
]})
w("think_flush_disabled", {"mode": "thinkingParser", "flushThinkingEnabled": False, "chunks": [
    "<thinking>held"
]})
w("think_flush_enabled", {"mode": "thinkingParser", "flushThinkingEnabled": True, "chunks": [
    "<thinking>held"
]})

# ---- ProviderRetryPolicy: HTTP / body / stream probes ----
w("retry_http_matrix", {"mode": "retry", "retryNumber": 1, "statusCode": 429, "body": None,
                        "streamErrorMessage": "unrelated"})
w("retry_http_body", {"mode": "retry", "retryNumber": 1, "statusCode": 401,
                      "body": "failed to generate: upstream", "streamErrorMessage": "unrelated"})
w("retry_http_fatal", {"mode": "retry", "retryNumber": 1, "statusCode": 401,
                       "body": "unauthorized", "streamErrorMessage": "unrelated"})
w("retry_upstream_negative", {"mode": "retry", "retryNumber": 1, "statusCode": 200,
                              "body": "something else", "streamErrorMessage": "unrelated"})
w("retry_stream_policy", {"mode": "retry", "retryNumber": 1, "statusCode": 200, "body": None,
                          "streamErrorMessage": "Failed To Generate upstream"})

print("batch2 scenarios written")
