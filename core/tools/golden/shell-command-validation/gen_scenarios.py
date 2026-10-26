#!/usr/bin/env python3
"""Deterministic scenario generator for shellCommandValidationError.

Each scenario exercises the validation function with deterministic inputs.
"""
from __future__ import annotations

import json
from pathlib import Path

HERE = Path(__file__).parent
OUT = HERE / "scenarios"
OUT.mkdir(exist_ok=True)

def write(name: str, scenario: dict):
    with open(OUT / (name + ".json"), "w") as f:
        json.dump(scenario, f, indent=2)
        f.write("\n")

def main():
    for f in OUT.glob("*.json"):
        f.unlink()

    # Valid command within limits
    write("01-valid_small", {
        "op": "shellCommandValidationError",
        "command": "ls -la",
        "workdir": "/tmp"
    })

    # Command exactly at limit (64KB = 65536 bytes)
    write("02-command_at_limit", {
        "op": "shellCommandValidationError",
        "command": "x" * 65536,
        "workdir": "/tmp"
    })

    # Command over limit (64KB + 1)
    write("03-command_over_limit", {
        "op": "shellCommandValidationError",
        "command": "x" * 65537,
        "workdir": "/tmp"
    })

    # Workdir exactly at limit (32KB = 32768 bytes)
    write("04-workdir_at_limit", {
        "op": "shellCommandValidationError",
        "command": "ls",
        "workdir": "x" * 32768
    })

    # Workdir over limit (32KB + 1)
    write("05-workdir_over_limit", {
        "op": "shellCommandValidationError",
        "command": "ls",
        "workdir": "x" * 32769
    })

    # Multi-byte UTF-8 command (check byte counting)
    # "€" is 3 bytes in UTF-8
    write("06-command_multibyte_utf8", {
        "op": "shellCommandValidationError",
        "command": "echo " + "€" * 21846,  # 5 + 21846*3 = 65543 bytes > 65536
        "workdir": "/tmp"
    })

    # Command with newline
    write("07-command_with_newline", {
        "op": "shellCommandValidationError",
        "command": "echo hello\necho world",
        "workdir": "/tmp"
    })

    # Workdir with multi-byte UTF-8 (check byte counting)
    write("08-workdir_multibyte_utf8", {
        "op": "shellCommandValidationError",
        "command": "ls",
        "workdir": "€" * 10923  # 10923*3 = 32769 bytes > 32768
    })

    # Empty command (valid - empty passes validation)
    write("09-empty_command", {
        "op": "shellCommandValidationError",
        "command": "",
        "workdir": "/tmp"
    })

    # Both over limit - command checked first
    write("10_both_over_limit", {
        "op": "shellCommandValidationError",
        "command": "x" * 65537,
        "workdir": "y" * 32769
    })

if __name__ == "__main__":
    main()