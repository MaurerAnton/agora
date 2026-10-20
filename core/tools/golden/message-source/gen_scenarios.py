#!/usr/bin/env python3
"""Generates the message-source scenarios. Deterministic; rerun after editing.

The scenarios feed stored JSON through the real MessageSource codec: `decode` prints the data
class, `encode` prints the canonical JSON (defaults omitted, unknown keys dropped), `sanitize`
pins the backup import gate, `readable` the ask_user text form, and `automation`/`constants` the
remaining public surface.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def decode(name, raw):
    case(name, op="decode", raw=raw)


def encode(name, raw):
    case(name, op="encode", raw=raw)


def sanitize(name, raw, participant):
    case(name, op="sanitize", raw=raw, participant=participant)


def readable(name, items):
    case(name, op="readable", items=items)


def automation(name, request_kind):
    case(name, op="automation", requestKind=request_kind)


def constants(name):
    case(name, op="constants")


def stored(kind, items=None, extra_top=None):
    obj = {"kind": kind}
    if items is not None:
        array = []
        for item in items:
            entry = {"question": item["question"]}
            if "answer" in item:
                entry["answer"] = item["answer"]
            array.append(entry)
        obj["askUser"] = array
    if extra_top:
        obj.update(extra_top)
    return json.dumps(obj, ensure_ascii=False, separators=(",", ":"))


QA = [{"question": "What now?", "answer": "Continue"}]

# ---- decode ----------------------------------------------------------------------------------
decode("decode_null", None)
decode("decode_empty", "")
decode("decode_blank", "   ")
decode("decode_task", '{"kind":"task"}')
decode("decode_loop", '{"kind":"loop"}')
decode("decode_ask_user_empty", '{"kind":"ask_user"}')
decode("decode_ask_user_one", stored("ask_user", QA))
decode("decode_ask_user_two", stored("ask_user", [
    {"question": "Q1", "answer": "A1"},
    {"question": "Q2", "answer": "A2"},
]))
decode("decode_answer_missing", stored("ask_user", [{"question": "Q"}]))
decode("decode_answer_null", '{"kind":"ask_user","askUser":[{"question":"Q","answer":null}]}')
decode("decode_answer_empty", stored("ask_user", [{"question": "Q", "answer": ""}]))
decode("decode_question_empty", stored("ask_user", [{"question": "", "answer": "A"}]))
decode("decode_unknown_top_key", '{"kind":"task","x":1}')
decode("decode_unknown_nested_key",
       '{"kind":"ask_user","askUser":[{"question":"Q","answer":"A","x":[1,2]}]}')
decode("decode_serial_task_upper", '{"kind":"TASK"}')
decode("decode_kind_missing", '{"x":1}')
decode("decode_kind_null", '{"kind":null}')
decode("decode_kind_number", '{"kind":3}')
decode("decode_kind_unknown", '{"kind":"bogus"}')
decode("decode_ask_user_null", '{"kind":"ask_user","askUser":null}')
decode("decode_ask_user_object", '{"kind":"ask_user","askUser":{}}')
decode("decode_item_not_object", '{"kind":"ask_user","askUser":[1]}')
decode("decode_item_question_missing", '{"kind":"ask_user","askUser":[{"answer":"A"}]}')
decode("decode_item_question_number", '{"kind":"ask_user","askUser":[{"question":5}]}')
decode("decode_item_question_null", '{"kind":"ask_user","askUser":[{"question":null}]}')
decode("decode_item_answer_number", '{"kind":"ask_user","askUser":[{"question":"Q","answer":5}]}')
decode("decode_item_answer_bool", '{"kind":"ask_user","askUser":[{"question":"Q","answer":true}]}')
decode("decode_task_with_items", stored("task", QA))
decode("decode_loop_with_items", stored("loop", QA))
decode("decode_extra_whitespace", '  { "kind" : "task" }  ')
decode("decode_trailing_junk", '{"kind":"task"}x')
decode("decode_two_values", '{"kind":"task"}{"kind":"loop"}')
decode("decode_bom", '\ufeff{"kind":"task"}')
decode("decode_escaped_text",
       stored("ask_user", [{"question": "a\"b\\c\nd\te", "answer": "é🙂"}]))
decode("decode_literal_control", '{"kind":"ask_user","askUser":[{"question":"a\x01b"}]}')
decode("decode_task_empty_items", '{"kind":"task","askUser":[]}')
decode("decode_single_quotes", "{'kind':'task'}")
decode("decode_unclosed", '{"kind":"task"')
decode("decode_deep_nested_unknown",
       '{"kind":"loop","meta":{"a":[1,{"b":null}]}}')

# ---- encode ----------------------------------------------------------------------------------
encode("encode_null", None)
encode("encode_invalid", "not json")
encode("encode_task", '{"kind":"task"}')
encode("encode_loop", '{"kind":"loop"}')
encode("encode_ask_user_empty", '{"kind":"ask_user"}')
encode("encode_ask_user_one", stored("ask_user", QA))
encode("encode_answer_missing", stored("ask_user", [{"question": "Q"}]))
encode("encode_answer_null", '{"kind":"ask_user","askUser":[{"question":"Q","answer":null}]}')
encode("encode_answer_empty", stored("ask_user", [{"question": "Q", "answer": ""}]))
encode("encode_question_escapes",
       stored("ask_user", [{"question": "a\"b\\c\nd\te", "answer": "tab\there"}]))
encode("encode_unicode", stored("ask_user", [{"question": "héllo 🙂", "answer": "日本語"}]))
encode("encode_control_chars",
       stored("ask_user", [{"question": "\u0001\u0007\u000b\u001f", "answer": "ok"}]))
encode("encode_input_whitespace", ' { "kind" : "ask_user" , "askUser" : [ { "question" : "Q" } ] } ')
encode("encode_task_empty_items", '{"kind":"task","askUser":[]}')
encode("encode_unknown_keys", '{"kind":"task","x":1,"askUser":[]}')
encode("encode_multi_items", stored("ask_user", [
    {"question": "Q1", "answer": "A1"},
    {"question": "Q2"},
    {"question": "Q3", "answer": ""},
]))
encode("encode_task_with_items", stored("task", QA))
encode("encode_question_only_spaces", stored("ask_user", [{"question": "  "}]))

# ---- sanitize --------------------------------------------------------------------------------
sanitize("sanitize_user_task", '{"kind":"task"}', "USER")
sanitize("sanitize_user_invalid", "not json", "USER")
sanitize("sanitize_user_null", None, "USER")
sanitize("sanitize_user_blank", "  ", "USER")
sanitize("sanitize_model_task", '{"kind":"task"}', "MODEL")
sanitize("sanitize_error_task", '{"kind":"task"}', "ERROR")
sanitize("sanitize_user_ask_user", stored("ask_user", QA), "USER")
sanitize("sanitize_user_ask_user_empty", '{"kind":"ask_user","askUser":[]}', "USER")
sanitize("sanitize_user_unknown_keys", '{"kind":"loop","x":1}', "USER")
sanitize("sanitize_user_task_with_items", stored("task", QA), "USER")
sanitize("sanitize_user_padded", '  {"kind":"task"}  ', "USER")

# ---- readable --------------------------------------------------------------------------------
readable("readable_one", [{"question": "Q", "answer": "A"}])
readable("readable_answer_missing", [{"question": "Q"}])
readable("readable_answer_empty", [{"question": "Q", "answer": ""}])
readable("readable_two", [
    {"question": "Q1", "answer": "A1"},
    {"question": "Q2", "answer": "A2"},
])
readable("readable_three", [
    {"question": "Q1", "answer": "A1"},
    {"question": "Q2"},
    {"question": "Q3", "answer": "A3"},
])
readable("readable_question_newline", [{"question": "line1\nline2", "answer": "A"}])
readable("readable_question_blank", [{"question": "", "answer": "A"}])
readable("readable_answer_newline", [{"question": "Q", "answer": "a\nb"}])
readable("readable_emoji", [{"question": "🙂?", "answer": "é"}])
readable("readable_pipes", [{"question": "a|b", "answer": "c;d"}])

# ---- automation / constants ------------------------------------------------------------------
automation("automation_task", "task")
automation("automation_loop", "loop")
automation("automation_other", "other")
automation("automation_empty", "")
automation("automation_null", None)
automation("automation_uppercase", "TASK")
automation("automation_padded", " task ")
constants("constants")


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
