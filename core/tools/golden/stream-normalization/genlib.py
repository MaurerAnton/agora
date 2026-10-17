# Shared helpers for the stream-normalization scenario generators.
# Tag-like payloads are always assembled from pieces, never one contiguous literal.
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
base = os.path.join(HERE, "scenarios")
os.makedirs(base, exist_ok=True)

TOOL_OPEN = "<tool_" + "call>"
TOOL_CLOSE = "<" + "/tool_" + "call>"
INVOKE_OPEN = "<" + "invoke"
THINK_OPEN = "<" + "thinking>"
THINK_CLOSE = "<" + "/thinking>"

CALC = ["calc", "search"]


def w(name, body):
    with open(os.path.join(base, name + ".json"), "w") as f:
        json.dump({"name": name, **body}, f, indent=2, ensure_ascii=True)
        f.write("\n")


def ev(type_, **kw):
    return {"type": type_, **kw}


def tagged(name, args_obj, extra=None):
    # extra: dict merged before "arguments" (e.g. {"id": "call_a"}).
    payload = {"name": name}
    if extra:
        payload.update(extra)
    payload["arguments"] = json.dumps(args_obj)
    return TOOL_OPEN + json.dumps(payload, separators=(",", ":")) + TOOL_CLOSE
