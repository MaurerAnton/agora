# Normalizer scenarios: routing, native passthrough, dedup, text tool recovery.
# Run from this directory: python3 gen_normalizer.py
from genlib import CALC, INVOKE_OPEN, THINK_CLOSE, THINK_OPEN, ev, tagged, w

# The XML-protocol payload is assembled from pieces so no protocol tag is one literal.
XML_CALL = (
    INVOKE_OPEN + ' name="calc">' + "<" + 'parameter name="a">1<' + '/parameter>' + "<" + '/invoke> done'
)

# ---- routing of plain text, structured thoughts, passthroughs ----
w("norm_plain_text", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text="Hello "), ev("text", text="world.")
]})

w("norm_inline_thinking", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text="Before "), ev("text", text=THINK_OPEN + " reasoning " + THINK_CLOSE),
    ev("text", text=" after."), ev("text", text="<|end|>done")
]})

w("norm_structured_thought", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text="plan: "),
    ev("thought", thought="step one", title="Plan", signature="sig-1"),
    ev("thought", thought="step two", title="Plan", signature="sig-1"),
    ev("text", text="answer")
]})

w("norm_empty_thought_metadata", {"mode": "normalizer", "tools": CALC, "events": [
    ev("thought", title="Only title"),
    ev("text", text="ok")
]})

w("norm_thought_interleave", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text=THINK_OPEN + "shared reasoning" + THINK_CLOSE + " tail"),
    ev("thought", thought="shared reasoning"),
    ev("text", text=" after")
]})

w("norm_passthrough_events", {"mode": "normalizer", "tools": CALC, "events": [
    ev("usage", tokenCount=42), ev("retrying", attempt=1, maxAttempts=5),
    ev("text", text="end")
]})

# ---- native calls and fingerprint dedup ----
w("norm_native_call_passthrough", {"mode": "normalizer", "tools": CALC, "events": [
    ev("toolCall", id="call_a", name="calc", arguments='{"a":1}'),
    ev("text", text="done")
]})

w("norm_native_multi_passthrough", {"mode": "normalizer", "tools": CALC, "events": [
    ev("toolCalls", calls=[
        ev("toolCall", id="call_a", name="calc", arguments='{"a":1}'),
        ev("toolCall", id="call_b", name="search", arguments='{"q":"x"}'),
    ])
]})

w("norm_tool_update_hosted_passthrough", {"mode": "normalizer", "tools": CALC, "events": [
    ev("toolCallUpdate", streamKey="key-1", id="call_a", name="calc", arguments='{"a":1}'),
    ev("hosted", streamKey="key-2", name="openai_search", arguments="{}"),
    ev("text", text="ok")
]})

w("norm_fingerprint_suppresses_text_call", {"mode": "normalizer", "tools": CALC, "events": [
    ev("toolCall", id="call_a", name="calc", arguments='{"b":2,"a":1}'),
    ev("text", text=tagged("calc", {"a": 1, "b": 2}))
]})

w("norm_duplicate_id_malformed", {"mode": "normalizer", "tools": CALC, "events": [
    ev("toolCall", id="call_a", name="calc", arguments='{"a":1}'),
    ev("text", text=tagged("calc", {"a": 2}, extra={"id": "call_a"}))
]})

# ---- text-rendered tool recovery ----
w("norm_text_tool_tagged", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text="Let me check."),
    ev("text", text=tagged("calc", {"a": 2})),
    ev("text", text="Done.")
]})

w("norm_text_tool_two_blocks", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text=tagged("calc", {}) + " then " + tagged("search", {"q": "x"}))
]})

w("norm_text_tool_bare_json", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text='{"name":"calc",'),
    ev("text", text='"arguments":"{}"}')
]})

w("norm_text_tool_malformed_json", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text='{"name":"calc","arguments":{broken}')
]})

w("norm_prose_json_fragment", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text='The result {"name":"not a tool"} stays prose.')
]})

w("norm_text_tool_invalid_id", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text=tagged("calc", {}, extra={"id": "bad id!"}))
]})

w("norm_xml_invoke_text", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text=" calling now " + XML_CALL)
]})

w("norm_tagged_residual_text", {"mode": "normalizer", "tools": CALC, "events": [
    ev("text", text=tagged("calc", {}) + "leftover text")
]})

w("norm_error_aborts_release", {"mode": "normalizer", "tools": CALC, "releaseTextTools": False, "events": [
    ev("text", text='{"name":"calc","arguments":"{}"'),
    ev("error")
]})

w("norm_authoritative_text", {"mode": "normalizer", "tools": CALC,
                              "nativeTextParsingAuthoritative": True, "events": [
    ev("text", text=tagged("calc", {}))
]})

w("norm_no_tools_passthrough", {"mode": "normalizer", "tools": [], "events": [
    ev("text", text=tagged("calc", {}))
]})

print("normalizer scenarios written")
