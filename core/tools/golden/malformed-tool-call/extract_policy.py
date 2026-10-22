#!/usr/bin/env python3
"""Lifts the malformed tool call policy out of the app tree with a widened visibility.

`api/util/MalformedToolCall.kt` declares its API `internal`, so a driver in another module cannot
call it. This script copies the file verbatim and only widens `internal` to the default (public)
visibility, so the golden stand compiles the real policy instead of a hand-written copy. No other
byte is changed.
"""
import os
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
APP = os.path.join(ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "api", "util")
OUT = os.path.join(HERE, "build")

SOURCES = ["MalformedToolCall.kt"]


def widen(text: str) -> str:
    out = []
    for line in text.split("\n"):
        stripped = line.lstrip()
        if stripped.startswith("internal "):
            indent = line[: len(line) - len(stripped)]
            line = indent + stripped[len("internal "):]
        out.append(line)
    return "\n".join(out)


def main() -> int:
    os.makedirs(OUT, exist_ok=True)
    for name in SOURCES:
        source = os.path.join(APP, name)
        if not os.path.exists(source):
            print("error: missing source %s" % source, file=sys.stderr)
            return 2
        text = open(source, encoding="utf-8").read()
        with open(os.path.join(OUT, name), "w", encoding="utf-8") as handle:
            handle.write(widen(text))
    print("Widened %d policy files" % len(SOURCES))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
