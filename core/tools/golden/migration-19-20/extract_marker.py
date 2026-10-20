#!/usr/bin/env python3
"""Lifts the truncation marker constant out of MessagePersistenceGuard.kt verbatim.

Migration19To20.kt only touches MessagePersistenceGuard.TRUNCATION_MARKER. Compiling the whole
guard would drag in ChatMessage.kt (Compose, Markdown, provider models), so the stand extracts the
single constant line instead. The extracted file is generated on every run and is not edited.
"""
import os
import re
import sys

HERE = os.path.dirname(os.path.abspath(__file__))
ROOT = os.path.abspath(os.path.join(HERE, "..", "..", "..", ".."))
SOURCE = os.path.join(
    ROOT, "app", "src", "main", "java", "com", "newoether", "agora", "model",
    "MessagePersistenceGuard.kt",
)
TARGET = os.path.join(HERE, "extracted", "MessagePersistenceGuardMarker.kt")


def main() -> int:
    with open(SOURCE, encoding="utf-8") as stream:
        text = stream.read()
    pattern = re.compile(r"^[ \t]*internal const val TRUNCATION_MARKER = .*$", re.MULTILINE)
    match = pattern.search(text)
    if match is None:
        raise SystemExit(f"error: TRUNCATION_MARKER not found in {SOURCE}")
    line = match.group(0).strip()

    os.makedirs(os.path.dirname(TARGET), exist_ok=True)
    with open(TARGET, "w", encoding="utf-8") as stream:
        stream.write("package com.newoether.agora.model\n\n")
        stream.write("object MessagePersistenceGuard {\n")
        stream.write(f"    {line}\n")
        stream.write("}\n")
    print(f"extracted TRUNCATION_MARKER into {TARGET}")
    return 0


if __name__ == "__main__":
    sys.exit(main())
