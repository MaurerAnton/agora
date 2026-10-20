#!/usr/bin/env python3
"""Lifts the two model enums Migration16To17.kt needs out of ChatMessage.kt verbatim."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
MODEL = os.path.join(ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "model")
TARGET = os.path.join(HERE, "extracted", "MigrationEnums.kt")

SOURCES = [
    ("ChatMessage.kt", ["Participant", "MessageStatus"]),
    ("RuntimeIdentity.kt", ["RunStatus", "RunEndReason"]),
]


def extract(path: str, name: str) -> str:
    with open(path, encoding="utf-8") as stream:
        text = stream.read()
    pattern = re.compile(r"^enum class " + name + r" \{\n(?:.*\n)*?^\}\n", re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        raise SystemExit(f"error: enum class {name} not found in {path}")
    return match.group(0).rstrip("\n")


def main() -> int:
    declarations = []
    for filename, names in SOURCES:
        source = os.path.join(MODEL, filename)
        for name in names:
            declarations.append(extract(source, name))
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", encoding="utf-8") as stream:
        stream.write("package com.newoether.agora.model\n\n")
        stream.write("\n\n".join(declarations))
        stream.write("\n")
    joined = ", ".join(name for _, names in SOURCES for name in names)
    print(f"extracted {joined} into {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
