#!/usr/bin/env python3
"""Generates the scenario corpus for the shell-tools golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def scenario(name, mode, **kw):
    out = {"name": name, "mode": mode}
    out.update(kw)
    ALL.append(out)


# ---- parseToolArgs / arg / boolArg ----------------------------------------------------------------
scenario("args_mixed_values", "toolArgs",
         arguments='{"s":"x","n":1,"b":true,"o":{"a":1},"arr":[1,2]}',
         probeKey="s")
scenario("args_blank_string_feeds_braces", "toolArgs", arguments="", probeKey="k")
scenario("args_whitespace_feeds_braces", "toolArgs", arguments="   ", probeKey="k")
scenario("args_malformed_returns_empty", "toolArgs", arguments="{oops", probeKey="k")
scenario("args_array_root_returns_empty", "toolArgs", arguments="[1,2]", probeKey="k")
scenario("args_primitive_root_returns_empty", "toolArgs", arguments="42", probeKey="k")
scenario("args_null_root_returns_empty", "toolArgs", arguments="null", probeKey="k")
scenario("args_duplicate_keys_last_wins_first_position", "toolArgs",
         arguments='{"a":1,"b":2,"a":3}', probeKey="a")
scenario("args_probe_null_value_is_literal", "toolArgs",
         arguments='{"z":null}', probeKey="z")
scenario("args_probe_object_value", "toolArgs",
         arguments='{"o":{"a":1}}', probeKey="o")
scenario("args_probe_array_value", "toolArgs",
         arguments='{"a":[1,2]}', probeKey="a")
scenario("args_probe_missing", "toolArgs", arguments='{"a":1}', probeKey="nope")
scenario("args_bool_true_literal", "toolArgs", arguments='{"b":true}', probeKey="b")
scenario("args_bool_false_literal", "toolArgs", arguments='{"b":false}', probeKey="b")
scenario("args_bool_quoted_true", "toolArgs", arguments='{"b":"true"}', probeKey="b")
scenario("args_bool_uppercase_false", "toolArgs", arguments='{"b":"TRUE"}', probeKey="b")
scenario("args_bool_number", "toolArgs", arguments='{"b":1}', probeKey="b")

# ---- jsonError --------------------------------------------------------------------------------------
scenario("error_full", "jsonError", type="net", message="boom", server="srv", command="ls")
scenario("error_blank_type_omitted", "jsonError", type="   ", message="boom")
scenario("error_without_optional_fields", "jsonError", type="net", message="boom")
scenario("error_special_chars", "jsonError", type="t", message='quo"te\nline\\slash',
         server=None, command=None)
scenario("error_empty_message", "jsonError", type="", message="")
scenario("error_server_only", "jsonError", type="t", message="m", server="box")

# ---- boundedFileReadJson -------------------------------------------------------------------------------
def file_read(name, content, max_length=None, **kw):
    payload = {
        "mode": "fileRead",
        "fileServer": kw.pop("server", "srv"),
        "filePath": kw.pop("path", "/tmp/f.txt"),
        "fileContent": content,
        "fileLines": kw.pop("lines", 0),
        "totalLines": kw.pop("total_lines", 10),
        "totalBytes": kw.pop("total_bytes", 100),
        "offset": kw.pop("offset", 0),
        "limit": kw.pop("limit", 0),
        "fileTruncated": kw.pop("truncated", False),
    }
    if max_length is not None:
        payload["maxLength"] = max_length
    scenario(name, **payload)


def utf16_len(text):
    return len(text.encode("utf-16-le")) // 2


def expected_full_json(content, lines_delta=0):
    """Mirrors boundedFileReadJson's encode(content=..., truncated=false) for the scenario's
    fixed server/path/counters — used to build exact-fit and pair-split max lengths."""
    payload = {
        "type": "file_read",
        "server": "srv",
        "path": "/tmp/f.txt",
        "content": content,
        "lines": (0 if content == "" else content.count("\n") + 1),
        "total_lines": 10,
        "total_bytes": 100,
        "returned_bytes": len(content.encode("utf-8")),
        "offset": 0,
        "limit": 0,
        "truncated": False,
    }
    return json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def wrappers_len(content):
    return utf16_len(expected_full_json(content)) - utf16_len(content)


file_read("file_small_fits_default_limit", "hello world")
file_read("file_empty_content", "", max_length=5000)
ascii_content = "x" * 200
file_read("file_exact_fit_boundary", ascii_content,
          max_length=len(expected_full_json(ascii_content)))
file_read("file_truncated_ascii", ascii_content, max_length=220)
file_read("file_tiny_limit_returns_over_limit_empty", ascii_content, max_length=5)
file_read("file_limit_one", ascii_content, max_length=1)
file_read("file_limit_zero_throws", ascii_content, max_length=0)
file_read("file_limit_negative_throws", ascii_content, max_length=-7)
file_read("file_multiline_lines_counted", "one\ntwo\nthree")
file_read("file_unicode_returned_bytes", "héllo €")
emoji_content = "AB😀CD😀EF"
# Three UTF-16 units would split the first surrogate pair; the guard backs off to "AB".
file_read("file_surrogate_pair_split_guard", emoji_content,
          max_length=wrappers_len(emoji_content) + 3)
# Five units: "AB" + the first pair exactly — a boundary that must keep the whole pair.
file_read("file_surrogate_pair_exact_boundary", emoji_content,
          max_length=wrappers_len(emoji_content) + 4)
file_read("file_big_unicode_truncation", "€" * 120, max_length=260)
file_read("file_metrics_passthrough", "abc", total_lines=42, total_bytes=999,
          offset=5, limit=77, truncated=True)


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
