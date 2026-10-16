# textParser and toolParser scenarios (StreamingTextToolCallParser + ToolCallTextParser).
# Run from this directory: python3 gen_texttool.py
from genlib import INVOKE_OPEN, TOOL_CLOSE, TOOL_OPEN, tagged, w

ANTML = "<" + "antml:invoke"
ANTML_PARAM = "<" + "antml:parameter"


def xml_call(name, params, attrs=""):
    body = ""
    for key, value in params:
        body += "<" + 'parameter name="' + key + '">' + value + "<" + "/parameter>"
    return INVOKE_OPEN + attrs + ' name="' + name + '">' + body + "<" + "/invoke>"


def antml_call(name, params):
    body = ""
    for key, value in params:
        body += ANTML_PARAM + ' name="' + key + '">' + value + "<" + "/antml:parameter>"
    return ANTML + ' name="' + name + '">' + body + "<" + "/antml:invoke>"


# ---- StreamingTextToolCallParser ----
w("textp_tagged_complete", {"mode": "textParser", "chunks": [
    "I will call. " + tagged("calc", {"a": 1}) + " Done."
]})

w("textp_tagged_malformed", {"mode": "textParser", "chunks": [
    TOOL_OPEN + '{"name":"calc","arguments":{broken}' + TOOL_CLOSE
]})

w("textp_bare_json", {"mode": "textParser", "chunks": [
    '{"name":"calc",', '"arguments":"{}"}'
]})

w("textp_prose_then_tagged", {"mode": "textParser", "chunks": [
    "Some prose first. Then " + TOOL_OPEN + '{"name":"calc","arguments":"'
]})

w("textp_xml_invoke", {"mode": "textParser", "chunks": [xml_call("calc", [("a", "1")])]})

w("textp_untagged_flush", {"mode": "textParser", "chunks": ["just prose", " continues"]})

w("textp_tagged_incomplete", {"mode": "textParser", "chunks": [
    TOOL_OPEN + '{"name":"calc","arg'
]})

w("textp_antml_tagged", {"mode": "textParser", "chunks": [
    antml_call("search", [("q", "cats")])
]})

w("textp_snapshot_prose_retry", {"mode": "textParser", "chunks": [
    '{"name":"nope"}', ' not a call at all'
]})

# ---- ToolCallTextParser.parse ----
w("parsep_tagged_function_nesting", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"function":{"name":"calc","arguments":"{}"}}' + TOOL_CLOSE})

w("parsep_tagged_id", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"id":"call_1","name":"calc","arguments":"{}"}' + TOOL_CLOSE})

w("parsep_array_atomic", {"mode": "toolParser", "content":
    '[{"name":"calc","arguments":"{}"},{"name":"search","arguments":"{}"}]'})

w("parsep_array_damaged", {"mode": "toolParser", "content":
    '[{"name":"calc","arguments":"{}"},{"name":"bad name","arguments":"{}"}]'})

w("parsep_xml_namespaced", {"mode": "toolParser", "content":
    antml_call("calc", [("a", "1")])})

w("parsep_xml_id_attribute", {"mode": "toolParser", "content":
    xml_call("calc", [("a", "1")], attrs=' id="call_x"')})

w("parsep_xml_unsafe_id", {"mode": "toolParser", "content":
    xml_call("calc", [("a", "1")], attrs=' id="bad id"')})

w("parsep_xml_entities", {"mode": "toolParser", "content":
    xml_call("calc", [("a", "&lt;b&gt;&amp;c")])})

w("parsep_id_conflict", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"id":"call_a","call_id":"call_b","name":"calc","arguments":"{}"}' + TOOL_CLOSE})

w("parsep_parameters_key", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"name":"calc","parameters":"{}"}' + TOOL_CLOSE})

w("parsep_unsafe_name", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"name":"bad name","arguments":"{}"}' + TOOL_CLOSE})

w("parsep_arguments_object", {"mode": "toolParser", "content":
    TOOL_OPEN + '{"name":"calc","arguments":{"a":1}}' + TOOL_CLOSE})

w("parsep_malformed_tagged_all_or_nothing", {"mode": "toolParser", "content":
    tagged("calc", {}) + " " + TOOL_OPEN + '{"name":"broken"}' + TOOL_CLOSE})

w("parsep_prose_not_tool", {"mode": "toolParser", "content": 'plain prose with "name" inside'})

print("textParser and toolParser scenarios written")
