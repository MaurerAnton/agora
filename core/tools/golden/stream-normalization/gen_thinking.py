# thinkingParser scenarios: chunk-boundary behavior, code fences, channel markers.
# Run from this directory: python3 gen_thinking.py
from genlib import THINK_CLOSE, THINK_OPEN, w

w("think_split_chunks", {"mode": "thinkingParser", "chunks": [
    "<think", "ing>secret", "<" + "/think", "ing>tail"
]})

w("think_fence_protection", {"mode": "thinkingParser", "chunks": [
    "```\n" + THINK_OPEN + "\n```\nreal "
]})

w("think_inline_code", {"mode": "thinkingParser", "chunks": ["`" + THINK_OPEN + "` and "]})

w("think_indented_code", {"mode": "thinkingParser", "chunks": ["   " + THINK_OPEN + "\ntext "]})

w("think_channel_markers", {"mode": "thinkingParser", "chunks": [
    "<|channel|>thought<|message|>deep", "<|channel|>final<|message|>final text"
]})

w("think_case_insensitive", {"mode": "thinkingParser", "chunks": [
    "<THINKING>caps", "<" + "/ThInKiNg>after"
]})

w("think_legacy_channel", {"mode": "thinkingParser", "chunks": [
    "<|channel>thought\nlegacy", "<channel|>after"
]})

w("think_partial_boundary", {"mode": "thinkingParser", "chunks": [
    "<think", "ing>hidden", "<" + "/think", "ing>shown"
]})

w("think_nested_channel_ends", {"mode": "thinkingParser", "chunks": [
    "<|channel|>analysis<|message|>first<|end|>after"
]})

w("think_disabled_passthrough", {"mode": "thinkingParser", "chunks": [
    "plain ", "thinking ", "<" + "/thinking> tail"
]})

print("thinking scenarios written")
