#!/usr/bin/env python3
"""Lifts the Participant enum out of model/ChatMessage.kt verbatim.

ChatMessage.kt drags the Compose/Room entity graph and kotlinx.serialization along, but
MessageSource.kt only needs Participant. This script copies the declaration exactly as written
(no retyping) into extracted/Participant.kt for the golden compile.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
SOURCE = os.path.join(
    ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "model", "ChatMessage.kt"
)
TARGET = os.path.join(HERE, "extracted", "Participant.kt")

PATTERN = re.compile(
    r"^enum class Participant \{\n(?:.*\n)*?\}\n", re.MULTILINE
)


def main() -> int:
    with open(SOURCE, encoding="utf-8") as stream:
        text = stream.read()
    match = PATTERN.search(text)
    if match is None:
        print("error: Participant enum not found in ChatMessage.kt", file=sys.stderr)
        return 1
    declaration = match.group(0).rstrip("\n")
    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", encoding="utf-8") as stream:
        stream.write("package com.newoether.agora.model\n\n")
        stream.write(declaration)
        stream.write("\n")
    print(f"extracted Participant ({declaration.count(chr(10)) + 1} lines) to {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
