#!/usr/bin/env python3
"""Deterministic scenario generator for the Claude chat importer.

`zip` scenarios are built by the Kotlin driver (java.util.zip) and frozen as fixtures/<stem>.zip so
the C++ port reads the same bytes; `text` scenarios are the raw export document bytes.
"""
from __future__ import annotations

import json
import os
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

TS = "2024-05-06T07:08:09Z"
TS_EARLY = "2023-01-02T03:04:05Z"
TS_OFFSET = "2024-05-06T09:08:09+02:00"
TS_MILLIS = "2024-05-06T07:08:09.250Z"


def conversation(uuid: str, name: str = "", summary: str = "", updated: str = TS,
                 messages: list | None = None, created: str = TS) -> dict:
    value = {"uuid": uuid}
    if name:
        value["name"] = name
    if summary:
        value["summary"] = summary
    value["created_at"] = created
    value["updated_at"] = updated
    value["chat_messages"] = messages or []
    return value


def message(uuid: str, sender: str = "human", text: str = "", created: str = TS,
            parent: str | None = None, content: list | None = None,
            attachments: list | None = None, files: list | None = None) -> dict:
    value = {"uuid": uuid, "sender": sender}
    if text:
        value["text"] = text
    value["created_at"] = created
    if parent is not None:
        value["parent_message_uuid"] = parent
    if content is not None:
        value["content"] = content
    if attachments is not None:
        value["attachments"] = attachments
    if files is not None:
        value["files"] = files
    return value


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def text_scenario(name: str, payload: str, selected=None) -> None:
    scenario = {"text": payload}
    if selected is not None:
        scenario["selectedIds"] = selected
    write(name, scenario)


def zip_scenario(name: str, entries: list, selected=None) -> None:
    scenario = {"zip": entries}
    if selected is not None:
        scenario["selectedIds"] = selected
    write(name, scenario)


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    simple = conversation("c1", name="First", messages=[
        message("m1", "human", "hello", parent="00000000-0000-4000-8000-000000000000"),
        message("m2", "assistant", "hi there", parent="m1"),
    ])
    other = conversation("c2", name="Second", updated=TS_EARLY, messages=[
        message("m3", "human", "second chat"),
    ])

    text_scenario("01-text-wrapper", json.dumps({"conversations": [simple, other]}, ensure_ascii=False))
    text_scenario("02-text-bare-array", json.dumps([simple], ensure_ascii=False))
    text_scenario("03-text-leading-whitespace", "   \n\t" + json.dumps({"conversations": [simple]}))
    text_scenario("04-text-unrecognised", "not json at all")
    text_scenario("05-text-empty", "")
    text_scenario("06-text-malformed", '{"conversations": [')
    text_scenario("07-text-empty-conversations", '{"conversations": []}')
    text_scenario("08-text-unknown-fields", json.dumps({
        "conversations": [dict(simple, extra_field=42, nested={"a": 1})],
        "unknown_root": True,
    }, ensure_ascii=False))
    text_scenario("09-text-null-coerced", json.dumps({
        "conversations": [{
            "uuid": "c9",
            "name": None,
            "summary": None,
            "created_at": None,
            "updated_at": None,
            "chat_messages": [{
                "uuid": "m9",
                "sender": None,
                "text": None,
                "created_at": None,
                "parent_message_uuid": None,
                "content": None,
            }],
        }],
    }))
    text_scenario("10-text-wrong-type", json.dumps({
        "conversations": [{"uuid": 42, "chat_messages": []}],
    }))
    text_scenario("11-text-missing-uuid", json.dumps({
        "conversations": [{"name": "no uuid"}],
    }))
    text_scenario("12-text-selection", json.dumps({"conversations": [simple, other]}),
                  selected=["c2"])
    text_scenario("13-text-selection-missing", json.dumps({"conversations": [simple, other]}),
                  selected=["nope"])
    text_scenario("14-text-timestamps", json.dumps({"conversations": [
        conversation("t1", name="Times", updated=TS_OFFSET, messages=[
            message("tm1", "human", "offset", created=TS_MILLIS),
            message("tm2", "assistant", "invalid", created="not-a-timestamp"),
        ]),
    ]}))
    text_scenario("15-text-content-blocks", json.dumps({"conversations": [
        conversation("b1", name="Blocks", messages=[
            message("bm1", "assistant", "raw text", content=[
                {"type": "thinking", "thinking": "secret reasoning", "text": "ignored"},
                {"type": "text", "text": "visible answer"},
            ]),
            message("bm2", "human", "no content blocks", content=[]),
        ]),
    ]}))
    text_scenario("16-text-citations", json.dumps({"conversations": [
        conversation("ct1", name="Cited", messages=[
            message("cm1", "assistant", "answer with citation", content=[
                {"type": "text", "text": "answer with citation", "citations": [
                    {"url": "https://example.com/doc", "title": "Doc", "uuid": "cite-1"},
                ]},
            ]),
        ]),
    ]}))
    text_scenario("17-text-attachments", json.dumps({"conversations": [
        conversation("a1", name="Files", messages=[
            message("am1", "human", "see attached", attachments=[
                {"file_name": "photo.png", "file_size": 12, "file_type": "image/png"},
                {"file_name": "notes.md", "file_size": 3, "file_type": "text/markdown",
                 "extracted_content": "# notes"},
                {"file_name": "mystery", "file_size": 1, "file_type": "application/octet-stream"},
            ], files=[
                {"file_uuid": "f-1", "file_name": "report.pdf", "file_size": 99,
                 "file_type": "application/pdf"},
            ]),
        ]),
    ]}))
    text_scenario("18-text-unicode", json.dumps({"conversations": [
        conversation("u1", name="Юникод 🙂", summary="сводка", messages=[
            message("um1", "human", "привет 🙂"),
        ]),
    ]}, ensure_ascii=False))
    text_scenario("19-text-many-messages", json.dumps({"conversations": [
        conversation("mm", name="Many", messages=[
            message("m-%02d" % index, "human" if index % 2 == 0 else "assistant",
                    "message %d" % index) for index in range(10)
        ]),
    ]}))
    text_scenario("20-text-title-fallbacks", json.dumps({"conversations": [
        conversation("tf1", messages=[message("tm", "human", "x")]),
        conversation("tf2", summary="a summary that is longer than fifty characters in total",
                     messages=[message("tm2", "human", "y")]),
        conversation("tf3", name="Named", summary="ignored", messages=[message("tm3", "human", "z")]),
    ]}))
    text_scenario("21-text-parent-zero-uuid", json.dumps({"conversations": [
        conversation("p1", name="Parents", messages=[
            message("pm1", "human", "root", parent="00000000-0000-4000-8000-000000000000"),
            message("pm2", "assistant", "child", parent="pm1"),
        ]),
    ]}))

    # ------------------------------------------------------------------ zip archives
    payload = json.dumps({"conversations": [simple, other]}, ensure_ascii=False)
    zip_scenario("30-zip-conversations-entry", [
        {"name": "conversations.json", "content": payload},
    ])
    zip_scenario("31-zip-fallback-entry", [
        {"name": "export.json", "content": payload},
    ])
    zip_scenario("32-zip-named-entry-wins", [
        {"name": "export.json", "content": json.dumps({"conversations": [other]})},
        {"name": "My-Conversations.json", "content": payload},
    ])
    zip_scenario("33-zip-non-json-entry-skipped", [
        {"name": "README.txt", "content": "not json"},
        {"name": "conversations.json", "content": payload},
    ])
    zip_scenario("34-zip-malformed-entry", [
        {"name": "conversations.json", "content": "{oops"},
    ])
    zip_scenario("35-zip-empty-conversations", [
        {"name": "conversations.json", "content": '{"conversations": []}'},
    ])
    zip_scenario("36-zip-no-json-entry", [
        {"name": "README.txt", "content": "nothing here"},
    ])
    zip_scenario("37-zip-stored-entry", [
        {"name": "conversations.json", "content": payload, "stored": "true"},
    ])
    zip_scenario("38-zip-selection", [
        {"name": "conversations.json", "content": payload},
    ], selected=["c1"])
    zip_scenario("39-zip-directory-and-entries", [
        {"name": "conversations.json", "content": payload},
        {"name": "other/data.json", "content": '{"conversations": []}'},
    ])

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
