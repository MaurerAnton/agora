#!/usr/bin/env python3
"""Generates the deterministic scenario corpus for the shell-file-edit golden stand.

The scenarios are plain JSON objects; each one is echoed by ShellFileEditTraceDump.kt and by the
C++ parity test. Run this after editing the list; run_golden.sh --freeze then rewrites golden/.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

scenarios = []


def add(name, scenario):
    scenarios.append((name, scenario))


def hexs(data):
    return data.hex()


# ---- lineCount (already pinned by shell-tools; cross-checked here) --------------------------
add("line-empty", {"op": "lineCount", "content": ""})
add("line-single", {"op": "lineCount", "content": "a"})
add("line-trailing-newline", {"op": "lineCount", "content": "a\n"})
add("line-only-newline", {"op": "lineCount", "content": "\n"})
add("line-two-newlines", {"op": "lineCount", "content": "\n\n"})
add("line-crlf", {"op": "lineCount", "content": "a\r\nb"})
add("line-cr-only", {"op": "lineCount", "content": "a\rb"})
add("line-unicode", {"op": "lineCount", "content": "héllo\n世界"})
add("line-multiline", {"op": "lineCount", "content": "a\nb\nc\n"})
add("line-emoji", {"op": "lineCount", "content": "😀\n😀"})

# ---- shellFileSha256 ------------------------------------------------------------------------
add("sha-empty", {"op": "sha256", "bytesHex": hexs(b"")})
add("sha-abc", {"op": "sha256", "bytesHex": hexs(b"abc")})
add("sha-hello", {"op": "sha256", "bytesHex": hexs(b"hello world")})
add("sha-utf8", {"op": "sha256", "bytesHex": hexs("héllo 世界 😀".encode())})
add("sha-newlines", {"op": "sha256", "bytesHex": hexs(b"a\r\nb\nc")})
add("sha-all-bytes", {"op": "sha256", "bytesHex": hexs(bytes(range(256)))})
add("sha-nul", {"op": "sha256", "bytesHex": hexs(b"\x00\x00\x00")})
add("sha-long", {"op": "sha256", "bytesHex": hexs(b"a" * 1000)})

# ---- completeUtf8PrefixLength ---------------------------------------------------------------
prefix_cases = [
    ("empty", b""),
    ("ascii", b"abc"),
    ("two-byte", "é".encode()),
    ("three-byte", "€".encode()),
    ("four-byte", "😀".encode()),
    ("cut-two", b"\xc3"),
    ("cut-three", b"\xe2\x82"),
    ("cut-four", b"\xf0\x9f\x98"),
    ("ascii-cut-two", b"a\xc3"),
    ("complete-two-cut-three", "é".encode() + b"\xe2\x82"),
    ("complete-three-stray-cont", "€".encode() + b"\x80"),
    ("all-continuation", b"\x80\x80"),
    ("invalid-lead-ff", b"\xff"),
    ("invalid-lead-c0", b"\xc0"),
    ("overlong-c0-80", b"\xc0\x80"),
    ("cut-four-b", b"\xf0\x90\x80"),
    ("complete-four-b", b"\xf0\x90\x80\x80"),
    ("ascii-four", b"abcd"),
    ("cut-f4", b"\xf4\x8f\xbf"),
    ("complete-f4", b"\xf4\x8f\xbf\xbf"),
    ("ascii-stray-cont", b"a\x80"),
    ("lead-two-ascii", b"\xc3A"),
]
for name, data in prefix_cases:
    add("prefix-" + name, {"op": "completeUtf8Prefix", "bytesHex": hexs(data)})

# ---- readBoundedShellOutput -----------------------------------------------------------------
def bounded(name, data, max_bytes=None, chunks=None):
    scenario = {"op": "readBoundedShellOutput", "bytesHex": hexs(data)}
    if max_bytes is not None:
        scenario["maxBytes"] = max_bytes
    scenario["chunks"] = chunks or []
    add(name, scenario)


bounded("bounded-empty", b"", 10)
bounded("bounded-ascii-under", b"hello", 10)
bounded("bounded-ascii-exact", b"hello", 5)
bounded("bounded-ascii-over", b"hello!", 5)
bounded("bounded-zero-limit", b"hello", 0)
bounded("bounded-empty-zero", b"", 0)
bounded("bounded-default-limit", b"hello")
bounded("bounded-two-byte-exact", "é".encode(), 2)
bounded("bounded-two-byte-cut", "é".encode(), 1)
bounded("bounded-ascii-two-byte-cut", "aé".encode(), 2)
bounded("bounded-three-byte-cut1", "€".encode(), 1)
bounded("bounded-three-byte-cut2", "€".encode(), 2)
bounded("bounded-three-byte-exact", "€".encode(), 3)
bounded("bounded-four-byte-cut3", "😀".encode(), 3)
bounded("bounded-four-byte-exact", "😀".encode(), 4)
bounded("bounded-ascii-four-byte-cut", "a😀".encode(), 2)
bounded("bounded-ascii-four-byte-exact", "a😀".encode(), 5)
bounded("bounded-invalid-ff", b"\xff", 1)
bounded("bounded-invalid-ff-cut", b"\xffa", 1)
bounded("bounded-overlong", b"\xc0\x80", 2)
bounded("bounded-overlong-cut", b"\xc0\x80", 1)
bounded("bounded-truncated-three", b"\xe2\x82", 2)
bounded("bounded-truncated-three-plus", b"\xe2\x82a", 2)
bounded("bounded-invalid-cont", b"\xe2AB", 3)
bounded("bounded-invalid-cont2", b"\xe2\x82A", 3)
bounded("bounded-stray-cont", b"a\x80", 2)
bounded("bounded-chunked", b"abcdefghij", 6, [1, 2, 3, 4])
bounded("bounded-chunked-exact", b"abcdefghij", 10, [1, 2, 3, 4])
bounded("bounded-negative", b"abc", -1)
bounded("bounded-large-default-chunks", b"a" * 9000, 8192)
bounded("bounded-nul-controls", b"\x00\x01\x07\x08", 4)

# ---- fileEditMatchRanges --------------------------------------------------------------------
def match(name, content, old):
    add(name, {"op": "matchRanges", "content": content, "oldString": old})


match("match-simple", "abc abc", "abc")
match("match-none", "abc", "zzz")
match("match-overlap", "aaaa", "aa")
match("match-aba", "abababa", "aba")
match("match-crlf-content-lf-old", "a\r\nb", "a\nb")
match("match-crlf-both", "a\r\nb", "a\r\nb")
match("match-lf-content-crlf-old", "a\nb", "a\r\nb")
match("match-newline-only", "a\r\nb", "\nb")
match("match-after-crlf", "a\r\nb", "b")
match("match-before-crlf", "a\r\nb", "a")
match("match-lone-cr", "a\rb", "a\rb")
match("match-lone-cr-vs-lf", "a\rb", "a\nb")
match("match-unicode", "😀x😀", "😀")
match("match-unicode-text", "a😀b", "😀b")
match("match-old-longer", "ab", "abc")
match("match-empty-old", "abc", "")
match("match-exact", "abc", "abc")
match("match-multiline", "one\ntwo\none\ntwo", "one")
match("match-crlf-multi", "x\r\ny\r\nx", "x")
match("match-old-crlf-multi", "a\r\nb\r\nc", "\r\n")
match("match-crlf-at-end", "a\r\n", "\r\n")
match("match-lf-at-end", "a\r\n", "\n")
match("match-empty-content", "", "a")
match("match-empty-content-empty-old", "", "")

# ---- replaceFileEditMatches -----------------------------------------------------------------
def replace(name, content, matches, new):
    add(name, {"op": "replaceMatches", "content": content, "matches": matches, "newString": new})


replace("replace-no-matches", "abc", [], "X")
replace("replace-one", "abc", [[1, 1]], "X")
replace("replace-two", "abcabc", [[0, 2], [3, 5]], "X")
replace("replace-crlf-file-lf-match", "a\r\nb", [[0, 2]], "x\ny")
replace("replace-crlf-file-lf-match-partial", "a\r\nb", [[0, 0]], "x\ny")
replace("replace-lf-file-crlf-new", "a\nb", [[0, 0]], "x\r\ny")
replace("replace-no-file-ending-crlf-new", "abc", [[1, 1]], "x\r\ny")
replace("replace-no-file-ending-lf-new", "abc", [[1, 1]], "x\ny")
replace("replace-no-newlines", "abc", [[1, 1]], "XYZ")
replace("replace-tie-crlf-first", "a\r\nb\nc", [[0, 0]], "x\ny")
replace("replace-tie-lf-first", "a\nb\r\nc", [[0, 0]], "x\ny")
replace("replace-matched-lf-overrides", "a\nb\r\nc", [[0, 2]], "x\r\ny")
replace("replace-matched-crlf-overrides", "a\r\nb\nc", [[0, 2]], "x\ny")
replace("replace-new-fallback", "abc", [[0, 2]], "x\r\ny")
replace("replace-lone-cr", "a\rb", [[1, 1]], "x\ny")
replace("replace-lone-cr-new", "a\rb", [[1, 1]], "x\ry")
replace("replace-trailing", "abc", [[2, 2]], "X")
replace("replace-all-content", "abc", [[0, 2]], "X")
replace("replace-invalid-overlap", "abcdef", [[2, 3], [1, 2]], "X")
replace("replace-invalid-oob", "abc", [[0, 3]], "X")
replace("replace-invalid-first-oob", "abc", [[3, 3]], "X")
replace("replace-empty-range", "abc", [[1, 0]], "X")
replace("replace-two-empty-ranges", "abc", [[1, 0], [2, 1]], "X")
replace("replace-empty-content-no-matches", "", [], "X")
replace("replace-crlf-new-lf", "a\r\nb", [[1, 2]], "x\ny")
replace("replace-lf-new-crlf", "a\nb", [[1, 1]], "x\r\ny")

# ---- editShellFileContent -------------------------------------------------------------------
def edit(name, content, old, new, replace_all):
    add(name, {
        "op": "edit",
        "content": content,
        "oldString": old,
        "newString": new,
        "replaceAll": replace_all,
    })


edit("edit-not-found", "abc", "z", "X", False)
edit("edit-one", "abc", "b", "X", False)
edit("edit-one-replace-all", "abc", "b", "X", True)
edit("edit-multi-no-replace-all", "abcabc", "abc", "X", False)
edit("edit-multi-replace-all", "abcabc", "abc", "X", True)
edit("edit-multi-three", "aaaa", "aa", "X", True)
edit("edit-crlf-preserve", "a\r\nb\r\nc", "b", "x\ny", False)
edit("edit-lf-preserve", "a\nb\nc", "b", "x\r\ny", False)
edit("edit-crlf-old-lf", "a\r\nb", "a\nb", "x", False)
edit("edit-old-crlf", "a\nb", "a\r\nb", "x", False)
edit("edit-empty-old", "abc", "", "X", False)
edit("edit-empty-content", "", "a", "X", False)
edit("edit-unicode", "😀x😀", "😀", "Y", True)
edit("edit-unicode-partial", "a😀b", "😀", "Z", False)
edit("edit-newline-only-old", "a\nb", "\n", "X", False)
edit("edit-crlf-only-old", "a\r\nb", "\r\n", "X", False)
edit("edit-newline-old-lf", "a\r\nb", "\n", "X", False)
edit("edit-no-line-ending-new", "abc", "b", "XYZ", False)
edit("edit-empty-new", "abc", "b", "", False)

# ---- preferredFileEditLineEnding ------------------------------------------------------------
def preferred(name, text):
    add(name, {"op": "preferredLineEnding", "text": text})


preferred("preferred-empty", "")
preferred("preferred-none", "abc")
preferred("preferred-lf", "a\nb")
preferred("preferred-crlf", "a\r\nb")
preferred("preferred-tie-crlf-first", "a\r\nb\nc")
preferred("preferred-tie-lf-first", "a\nb\r\nc")
preferred("preferred-crlf-majority", "a\r\nb\r\nc\nd")
preferred("preferred-lf-majority", "a\nb\nc\r\nd")
preferred("preferred-lone-cr", "a\rb")
preferred("preferred-lone-cr-then-lf", "a\rb\nc")
preferred("preferred-trailing-cr", "a\r\nb\r")
preferred("preferred-only-lf", "\n")
preferred("preferred-only-crlf", "\r\n")
preferred("preferred-only-cr", "\r")
preferred("preferred-double-lf", "a\n\nb")
preferred("preferred-double-crlf", "a\r\n\r\nb")

# ---- withFileEditLineEnding -----------------------------------------------------------------
def ending(name, text, line_ending):
    add(name, {"op": "withLineEnding", "text": text, "lineEnding": line_ending})


ending("ending-no-lf", "abc", "\r\n")
ending("ending-lf-to-lf", "a\nb", "\n")
ending("ending-lf-to-crlf", "a\nb", "\r\n")
ending("ending-crlf-to-lf", "a\r\nb", "\n")
ending("ending-crlf-to-crlf", "a\r\nb", "\r\n")
ending("ending-lone-cr", "a\rb\nc", "\r\n")
ending("ending-trailing", "a\nb\n", "\r\n")
ending("ending-consecutive", "a\r\n\r\nb", "\n")
ending("ending-empty-text", "", "\r\n")
ending("ending-empty-ending", "a\nb", "")
ending("ending-cr-ending", "a\nb", "\r")
ending("ending-mixed", "a\r\nb\nc", "\r\n")
ending("ending-only-cr-text", "a\rb", "\r\n")
ending("ending-only-lf-text", "\n", "\r\n")
ending("ending-only-crlf-text", "\r\n", "\n")


def main():
    os.makedirs(OUT, exist_ok=True)
    for old in os.listdir(OUT):
        if old.endswith(".json"):
            os.unlink(os.path.join(OUT, old))
    for index, (name, scenario) in enumerate(scenarios, start=1):
        path = os.path.join(OUT, f"{index:03d}-{name}.json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(scenario, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"wrote {len(scenarios)} scenarios to {OUT}")


if __name__ == "__main__":
    main()
