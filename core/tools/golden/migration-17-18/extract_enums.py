#!/usr/bin/env python3
"""Lifts Participant out of ChatMessage.kt verbatim for Migration17To18.kt."""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
MODEL = os.path.join(ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "model")
TARGET = os.path.join(HERE, "extracted", "MigrationEnums.kt")


def extract(path: str, name: str) -> str:
    with open(path, encoding="utf-8") as stream:
        text = stream.read()
    pattern = re.compile(r"^enum class " + name + r" \{\n(?:.*\n)*?^\}\n", re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        raise SystemExit(f"error: enum class {name} not found in {path}")
    return match.group(0).rstrip("\n")


def main() -> int:
    source = os.path.join(MODEL, "ChatMessage.kt")
    declaration = extract(source, "Participant")
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", encoding="utf-8") as stream:
        stream.write("package com.newoether.agora.model\n\n")
        stream.write(declaration)
        stream.write("\n")
    print(f"extracted Participant into {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
