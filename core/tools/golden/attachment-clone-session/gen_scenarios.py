#!/usr/bin/env python3
"""Deterministic scenario generator for the attachment clone session."""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

META = json.dumps({
    "items": [
        {"type": "image", "file_name": "a.png", "originalUri": "file://platform_clone_workspace/sources/a.png"},
        {"type": "file", "file_name": "b.txt", "originalUri": "https://example.com/b.txt"},
        {"type": "file", "file_name": "c.txt"},
    ]
})


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    write("01-clone-images-and-meta", {
        "sources": [
            {"name": "a.png", "content": "png-bytes"},
            {"name": "b.txt", "content": "text-bytes"},
        ],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "text": "with attachments",
                "images": ["platform_clone_workspace/sources/a.png"],
                "attachmentMeta": META,
            },
        ],
    })

    write("02-clone-same-owner-shares-file", {
        "sources": [{"name": "a.png", "content": "png-bytes"}],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
            {
                "id": "m2",
                "conversationId": "c1",
                "ownerKey": "owner-1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
            {
                "id": "m3",
                "conversationId": "c1",
                "ownerKey": "owner-1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
        ],
    })

    write("03-clone-different-owner-separates-file", {
        "sources": [{"name": "a.png", "content": "png-bytes"}],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "ownerKey": "owner-1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
            {
                "id": "m2",
                "conversationId": "c1",
                "ownerKey": "owner-2",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
        ],
    })

    write("04-clone-missing-source-keeps-path", {
        "sources": [],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "images": ["platform_clone_workspace/sources/missing.png"],
            },
        ],
    })

    write("05-clone-rollback-removes-files", {
        "sources": [{"name": "a.png", "content": "png-bytes"}],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
        ],
        "commit": "false",
    })

    write("06-clone-commit-keeps-files", {
        "sources": [{"name": "a.png", "content": "png-bytes"}],
        "messages": [
            {
                "id": "m1",
                "conversationId": "c1",
                "images": ["platform_clone_workspace/sources/a.png"],
            },
        ],
        "commit": "true",
    })

    write("07-clone-standalone-meta", {
        "sources": [{"name": "a.png", "content": "png-bytes"}],
        "messages": [],
        "standaloneMeta": {"raw": META},
    })

    write("08-clone-no-attachments", {
        "sources": [],
        "messages": [
            {"id": "m1", "conversationId": "c1", "text": "plain", "images": []},
        ],
    })

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
