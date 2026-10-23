#!/usr/bin/env python3
"""Lifts the NativeGraphEntrySource interface verbatim from NativeConversationGraphSource.kt.

NativeConversationGraphSource.kt itself drags in android.util.JsonReader, which is an Android
platform type this stand does not substitute; the three-method interface is the only surface
NativeBackupArchive implements, so it is extracted byte-for-byte here.
"""
import sys

def main() -> None:
    source, target = sys.argv[1], sys.argv[2]
    with open(source, encoding="utf-8") as handle:
        text = handle.read()
    start = text.index("internal interface NativeGraphEntrySource")
    end = text.index("}", start) + 1
    with open(target, "w", encoding="utf-8") as handle:
        handle.write("package com.newoether.agora.data\n\n")
        handle.write("import java.io.InputStream\n\n")
        handle.write(text[start:end])
        handle.write("\n")

if __name__ == "__main__":
    main()
