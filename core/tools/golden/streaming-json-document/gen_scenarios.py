#!/usr/bin/env python3
"""Generates the streaming-json-document scenarios. Deterministic; rerun after editing.

Three operations: `parse` pins the prefix JSON parser (status, error offsets in UTF-16 units,
structural trees with open leaves), `hints` pins the bounded tool-argument hint extraction for
every ToolKind branch, and `normalize` pins the subject normalizer.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def parse(name, source):
    case(name, op="parse", source=source)


def hints(name, kind, raw):
    case(name, op="hints", kind=kind, rawArguments=raw)


def normalize(name, value, max_chars=120):
    case(name, op="normalize", value=value, max=max_chars)


# ---- parser: document statuses ----------------------------------------------------------------
parse("doc_empty", "")
parse("doc_space_only", " ")
parse("doc_ws_only_multichar", "\t\n ")
parse("doc_ws_then_value", "  42  ")
parse("doc_crlf_whitespace", "{\r\n\"a\":\r\n1\r\n}")
parse("doc_object_complete", '{"a":1}')
parse("doc_object_empty", "{}")
parse("doc_object_empty_trailing_ws", "{} ")
parse("doc_object_partial_key", '{"')
parse("doc_object_partial_value_null", '{"a":')
parse("doc_object_partial_value", '{"a":1')
parse("doc_object_after_comma", '{"a":1,')
parse("doc_object_trailing_comma", '{"a":1,}')
parse("doc_object_key_not_string", "{a:1}")
parse("doc_object_no_colon", '{"a" 1}')
parse("doc_object_missing_comma", '{"a":1 "b":2}')
parse("doc_object_bad_after_value", '{"a":1 2}')
parse("doc_object_key_with_escape", '{"a\\"b":1}')
parse("doc_object_key_only", '{"a"')
parse("doc_object_nested_partial", '{"a":{"b":')
parse("doc_object_keyword_value_partial", '{"a":nul')
parse("doc_object_deep_nest", '[1,{"b":[2,{"c":')
parse("doc_array_complete", "[1,2,3]")
parse("doc_array_empty", "[]")
parse("doc_array_after_comma", "[1,")
parse("doc_array_trailing_comma", "[1,]")
parse("doc_array_missing_comma", "[1 2]")
parse("doc_array_mixed", '[1,"a",true,null,{},[]]')
parse("doc_array_nested_partial", '[["a')
parse("doc_string_root", '"abc"')
parse("doc_string_partial", '"abc')
parse("doc_string_empty", '""')
parse("doc_string_escapes", '"\\"\\\\\\/\\b\\f\\n\\r\\t"')
parse("doc_string_unicode_escape", '"\\u0041\\u00e9"')
parse("doc_string_unicode_surrogate_pair", '"\\ud83d\\ude42"')
parse("doc_string_trailing_backslash", '"abc\\')
parse("doc_string_bad_escape", '"\\q"')
parse("doc_string_bad_unicode", '"\\u12g4"')
parse("doc_string_short_unicode_truncated", '"\\u12')
parse("doc_string_control_char", '{"a":"x\u0001y"}')
parse("doc_string_non_ascii", '"h\u00e9llo \U0001f642"')
parse("doc_error_offset_after_emoji", '"\U0001f642\\q"')
parse("doc_error_offset_after_accent", '"\u00e9\\q"')
parse("doc_keyword_true", "true")
parse("doc_keyword_false", "false")
parse("doc_keyword_null", "null")
parse("doc_keyword_partial", "tru")
parse("doc_keyword_mismatch", "trX")
parse("doc_number_int", "42")
parse("doc_number_negative", "-42")
parse("doc_number_fraction", "4.25")
parse("doc_number_exponent", "1e10")
parse("doc_number_exponent_sign", "-0.5e-10")
parse("doc_number_partial_fraction", "12.")
parse("doc_number_partial_exponent", "1e")
parse("doc_number_partial_exponent_sign", "1e+")
parse("doc_number_leading_zero", "01")
parse("doc_number_bad_fraction", "1.x")
parse("doc_number_bad_exponent", "1ex")
parse("doc_number_minus_only", "-")
parse("doc_number_minus_garbage", "-x")
parse("doc_multi_roots_ws", "1 2")
parse("doc_multi_roots_cr", "1\r\n2")
parse("doc_multi_roots_ws_objects", "{} []")
parse("doc_multi_roots_no_ws", "{}[]")
parse("doc_multi_roots_then_garbage", "{} x")
parse("doc_bom_not_whitespace", "\ufeff{}")

# ---- hint resolver -----------------------------------------------------------------------------
hints("hints_memory_read_single", "MEMORY_READ", '{"names":["note.md"]}')
hints("hints_memory_read_partial_name", "MEMORY_READ", '{"names":["no')
hints("hints_memory_read_two_names", "MEMORY_READ", '{"names":["a.md","b.md"]}')
hints("hints_memory_read_empty_name", "MEMORY_READ", '{"names":[""]}')
hints("hints_memory_read_blank_name", "MEMORY_READ", '{"names":["  "]}')
hints("hints_memory_read_empty_array", "MEMORY_READ", '{"names":[]}')
hints("hints_memory_read_names_truncated", "MEMORY_READ", '{"names":[')
hints("hints_memory_read_fallback_name", "MEMORY_READ", '{"name":"x.md"}')
hints("hints_skill_read_single", "SKILL_READ", '{"names":["skill.md"]}')
hints("hints_skill_read_blank", "SKILL_READ", '{"names":[" "]}')
hints("hints_memory_list", "MEMORY_LIST", '{"names":["a","b"]}')
hints("hints_skill_list", "SKILL_LIST", '{"names":["s1"]}')
hints("hints_memory_create", "MEMORY_CREATE", '{"name":"x.md","content":"hi"}')
hints("hints_memory_edit", "MEMORY_EDIT", '{"name":"y.md"}')
hints("hints_memory_delete", "MEMORY_DELETE", '{"name":"z.md"}')
hints("hints_skill_create", "SKILL_CREATE", '{"name":"s.md"}')
hints("hints_skill_edit", "SKILL_EDIT", '{"name":"s2.md"}')
hints("hints_skill_delete", "SKILL_DELETE", '{"name":"s3.md"}')
hints("hints_web_search", "WEB_SEARCH", '{"query":"cats"}')
hints("hints_conversation_search", "CONVERSATION_SEARCH", '{"query":"hello"}')
hints("hints_web_fetch", "WEB_FETCH", '{"url":"https://example.com"}')
hints("hints_conversation_read", "CONVERSATION_READ", '{"conversation_id":"c7"}')
hints("hints_shell_execute", "SHELL_EXECUTE", '{"command":"ls -la","cwd":"/tmp"}')
hints("hints_shell_job_get", "SHELL_JOB_GET", '{"job_id":"j1"}')
hints("hints_shell_job_wait", "SHELL_JOB_WAIT", '{"job_id":"j2"}')
hints("hints_shell_job_stop", "SHELL_JOB_STOP", '{"job_id":"j3"}')
hints("hints_file_read", "FILE_READ", '{"path":"/etc/hosts"}')
hints("hints_file_write", "FILE_WRITE", '{"path":"/tmp/x.txt"}')
hints("hints_file_edit", "FILE_EDIT", '{"path":"a/b.kt"}')
hints("hints_image_view", "IMAGE_VIEW", '{"path":"img.png"}')
hints("hints_file_glob", "FILE_GLOB", '{"pattern":"**/*.kt"}')
hints("hints_file_grep", "FILE_GREP", '{"pattern":"TODO"}')
hints("hints_image_generate", "IMAGE_GENERATE", '{"prompt":"a cat"}')
hints("hints_task_create", "TASK_CREATE", '{"name":"task one"}')
hints("hints_task_delete_id", "TASK_DELETE", '{"id_or_name":"t1"}')
hints("hints_task_delete_name", "TASK_DELETE", '{"name":"t2"}')
hints("hints_task_delete_task_id", "TASK_DELETE", '{"task_id":"t3"}')
hints("hints_task_delete_none", "TASK_DELETE", '{"other":1}')
hints("hints_task_list", "TASK_LIST", '{"names":["a"]}')
hints("hints_loop_start", "LOOP_START", '{"name":"x"}')
hints("hints_ask_user", "ASK_USER", '{"question":"q?"}')
hints("hints_unknown_kind", "UNKNOWN", '{"name":"x"}')
hints("hints_server_operation_destination", "MEMORY_CREATE",
      '{"name":"a","server":"srv","operation":"PUT","new_name":"b"}')
hints("hints_operation_shell", "SHELL_EXECUTE", '{"command":"pwd","operation":"run"}')
hints("hints_raw_null", "FILE_READ", None)
hints("hints_raw_blank", "FILE_READ", "   ")
hints("hints_raw_not_json", "FILE_READ", "hello world")
hints("hints_raw_open_brace", "FILE_READ", "{")
hints("hints_prefix_fits", "MEMORY_CREATE", '{"name":"' + "a" * 4084 + '"}')
hints("hints_prefix_cut_mid_string", "MEMORY_CREATE",
      '{"name":"' + "a" * 4086 + '"}')
hints("hints_prefix_boundary_excludes_tail", "MEMORY_CREATE",
      '{"name":"x","content":"' + "A" * 4071 + '","new_name":"dst"}')
hints("hints_prefix_boundary_includes_tail", "MEMORY_CREATE",
      '{"name":"x","content":"' + "A" * 4055 + '","new_name":"dst"}')

# ---- normalize --------------------------------------------------------------------------------
normalize("normalize_null", None)
normalize("normalize_empty", "")
normalize("normalize_blank", "   ")
normalize("normalize_collapse", "  a b  ")
normalize("normalize_newline_tab", "a\n\tb   c")
normalize("normalize_nbsp_trimmed", "\u00a0x\u00a0")
normalize("normalize_nbsp_only", "\u00a0")
normalize("normalize_vertical_tab_form_feed", "a\u000bb\fc")
normalize("normalize_exact_120", "x" * 120)
normalize("normalize_over_120", "x" * 121)
normalize("normalize_unicode_length_120", "\U0001f642" * 59 + "xy")
normalize("normalize_unicode_length_121", "\U0001f642" * 59 + "xyz")
normalize("normalize_small_max", "abcdefg", max_chars=5)
normalize("normalize_single_char_max", "ab", max_chars=1)


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
