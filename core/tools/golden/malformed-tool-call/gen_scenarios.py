#!/usr/bin/env python3
"""Deterministic scenario generator for the malformed tool call policy."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-malformed-basic", {
        "requests": [
            {"cause": "the tool name was empty", "originalName": None,
             "originalArguments": None, "streamKey": None, "signature": None},
        ],
        "results": [
            '{"error":"the tool name was empty"}',
        ],
    })

    write("02-malformed-with-originals", {
        "requests": [
            {"cause": "duplicate tool call id", "originalName": "search",
             "originalArguments": '{"q":"x"}', "streamKey": "stream-7",
             "signature": "sig-1"},
        ],
        "results": [
            '{"error":"duplicate tool call id","original_name":"search",'
            '"original_arguments":"{\\"q\\":\\"x\\"}"}',
        ],
    })

    write("03-malformed-blank-originals", {
        "requests": [
            {"cause": "arguments were not an object", "originalName": "",
             "originalArguments": "", "streamKey": "  ", "signature": ""},
        ],
        "results": [
            '{"error":"arguments were not an object"}',
        ],
    })

    write("04-malformed-long-originals", {
        "requests": [
            {"cause": "x" * 500, "originalName": "n" * 500,
             "originalArguments": "a" * 5000, "streamKey": None, "signature": None},
        ],
        "results": [
            '{"error":"' + "x" * 500 + '","original_name":"' + "n" * 500 +
            '","original_arguments":"' + "a" * 5000 + '"}',
        ],
    })

    write("05-malformed-result-texts", {
        "requests": [
            {"cause": "cause", "originalName": None, "originalArguments": None,
             "streamKey": None, "signature": None},
        ],
        "results": [
            "not json at all",
            "",
            "{}",
            '{"error":""}',
            '{"error":"e","original_name":"","original_arguments":""}',
            '{"error":42}',
            '{"other":"x"}',
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
