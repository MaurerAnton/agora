#!/usr/bin/env python3
"""Extracts the ToolKind enum from ToolPresentation.kt into the stand, byte for byte.

The hint resolver (StreamingToolArgumentHints.kt) switches on ToolKind, but ToolPresentation.kt
pulls kotlinx.serialization and ChatMessage along, which the stand must not compile. Lifting is
done declaration-for-declaration by script — the enum block below is copied, never retyped — and
this script fails loudly if the source block moves or changes shape.

Usage: python3 extract_tool_kind.py   (rewrites extracted/ToolKind.kt, exits 1 on drift)
"""
import pathlib
import re
import sys

HERE = pathlib.Path(__file__).resolve().parent
ROOT = HERE.parents[3]  # .../agora-work
SOURCE = ROOT / "app/src/main/java/com/newoether/agora/ui/chat/message/ToolPresentation.kt"
OUTPUT = HERE / "extracted/ToolKind.kt"

text = SOURCE.read_text(encoding="utf-8")
match = re.search(r"(?ms)^internal enum class ToolKind \{\n.*?^\}$", text)
if not match:
    sys.exit("extract_tool_kind: ToolKind enum block not found in ToolPresentation.kt")
block = match.group(0)
if not block.rstrip().endswith("}"):
    sys.exit("extract_tool_kind: extracted block does not end with a closing brace")

package_match = re.search(r"(?m)^package .*$", text)
package = package_match.group(0) if package_match else "package com.newoether.agora.ui.chat.message"

output = f"{package}\n\n{block}\n"
OUTPUT.parent.mkdir(parents=True, exist_ok=True)
OUTPUT.write_text(output, encoding="utf-8")
print(f"extracted ToolKind ({len(block.splitlines())} lines) to {OUTPUT}")
