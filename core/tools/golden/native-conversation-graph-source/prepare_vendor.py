#!/usr/bin/env python3
"""Stages the vendored android.util.Json* sources for javac.

vendor/ holds the files verbatim from android.googlesource.com (frameworks/base
core/java/android/util/{JsonReader,JsonWriter,JsonToken,MalformedJsonException}.java and
core/java/com/android/internal/util/StringPool.java, fetched from refs/heads/main). The AOSP
sources carry `@android.ravenwood.annotation.RavenwoodKeepWholeClass` — an Android test-harness
annotation that does not exist on a plain JDK — so the single documented build-time edit is
dropping lines that mention `ravenwood`. Nothing else is touched; the vendored files stay the
app-reachable platform contract this stand pins (error texts, line/column arithmetic, lenient
rules, Double.toString emission).
"""
from __future__ import annotations

import os
import sys


def main() -> None:
    if len(sys.argv) != 3:
        raise SystemExit("usage: prepare_vendor.py <vendor-dir> <output-dir>")
    source_dir, output_dir = sys.argv[1], sys.argv[2]
    os.makedirs(output_dir, exist_ok=True)
    copied = 0
    for name in sorted(os.listdir(source_dir)):
        if not name.endswith(".java"):
            continue
        with open(os.path.join(source_dir, name), encoding="utf-8") as handle:
            lines = handle.readlines()
        kept = [line for line in lines if "ravenwood" not in line]
        with open(os.path.join(output_dir, name), "w", encoding="utf-8", newline="") as handle:
            handle.writelines(kept)
        copied += 1
    if copied == 0:
        raise SystemExit("no vendored java files found in " + source_dir)
    print(f"staged {copied} vendored android.util sources")


if __name__ == "__main__":
    main()
