#!/usr/bin/env python3
"""Deterministic scenario generator for the GPT chat importer.

`zip` scenarios are built by the Kotlin driver (java.util.zip) and frozen as fixtures/<stem>.zip so
the C++ port reads the same bytes; `text` scenarios are the raw conversations.json bytes.
"""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

ROOT = "client-created-root"


def message(msg_id: str, role: str = "user", *, parts=None, content_type: str = "text",
            text: str | None = None, result: str | None = None, thoughts: list | None = None,
            reasoning: str | None = None, create_time: float | None = 1700000000.0,
            metadata: dict | None = None) -> dict:
    content: dict = {"content_type": content_type}
    if parts is not None:
        content["parts"] = parts
    if text is not None:
        content["text"] = text
    if result is not None:
        content["result"] = result
    if reasoning is not None:
        content["content"] = reasoning
    if thoughts is not None:
        content["thoughts"] = thoughts
    value: dict = {"id": msg_id, "author": {"role": role}, "content": content}
    if create_time is not None:
        value["create_time"] = create_time
    if metadata is not None:
        value["metadata"] = metadata
    return value


def node(node_id: str, msg: dict | None = None, parent: str | None = ROOT) -> dict:
    value: dict = {"id": node_id}
    if msg is not None:
        value["message"] = msg
    value["parent"] = parent
    return value


def conversation(conversation_id: str, mapping: dict, *, title: str = "Chat",
                 current_node: str | None = None, update_time: float = 1700000005.5,
                 create_time: float = 1700000000.0) -> dict:
    value: dict = {"conversation_id": conversation_id, "title": title,
                   "create_time": create_time, "update_time": update_time,
                   "mapping": mapping}
    if current_node is not None:
        value["current_node"] = current_node
    return value


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def text_scenario(name: str, payload, selected=None) -> None:
    scenario = {"text": payload if isinstance(payload, str) else json.dumps(payload, ensure_ascii=False)}
    if selected is not None:
        scenario["selectedIds"] = selected
    write(name, scenario)


def zip_scenario(name: str, entries: list, selected=None) -> None:
    scenario = {"zip": entries}
    if selected is not None:
        scenario["selectedIds"] = selected
    write(name, scenario)


def simple_mapping() -> dict:
    return {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["hello"]), "n0"),
        "n2": node("n2", message("m2", "assistant", parts=["hi there"]), "n1"),
    }


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    text_scenario("01-text-simple", [conversation("c1", simple_mapping())])
    text_scenario("02-text-empty-mapping", [conversation("c2", {})])
    text_scenario("03-text-empty-array", [])
    text_scenario("04-text-not-an-array", {"conversations": []})
    text_scenario("05-text-malformed", "[{")
    text_scenario("06-text-unknown-fields", [{
        **conversation("c3", simple_mapping()),
        "unknown": 1,
        "extra": {"nested": True},
    }])
    text_scenario("07-text-branching", [conversation("c4", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["start"]), "n0"),
        "n2": node("n2", message("m2", "assistant", parts=["first branch"]), "n1"),
        "n3": node("n3", message("m3", "assistant", parts=["second branch"]), "n1"),
    }, current_node="n3")])
    text_scenario("08-text-cycle", [conversation("c5", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["x"]), "n2"),
        "n2": node("n2", message("m2", "assistant", parts=["y"]), "n1"),
    })])
    text_scenario("09-text-dangling-parent", [conversation("c6", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["x"]), "missing"),
        "n2": node("n2", message("m2", "assistant", parts=["y"]), "n1"),
    })])
    text_scenario("10-text-invalid-current-node", [conversation("c7", simple_mapping(),
                                                               current_node="nope")])
    text_scenario("11-text-ephemeral-nodes", [conversation("c8", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["question"]), "n0"),
        "n2": node("n2", message("m2", "thoughts", content_type="thoughts",
                                 thoughts=[{"content": "thinking hard", "summary": "plan"}]), "n1"),
        "n3": node("n3", message("m3", "assistant", parts=["answer"]), "n2"),
        "n4": node("n4", message("m4", "code", content_type="code", text="print(1)"), "n3"),
        "n5": node("n5", message("m5", "tool", content_type="execution_output", result="42"), "n4"),
    })])
    text_scenario("12-text-reasoning-recap", [conversation("c9", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["question"]), "n0"),
        "n2": node("n2", message("m2", "reasoning_recap", content_type="reasoning_recap",
                                 reasoning="a recap",
                                 metadata={"finished_duration_sec": 3}), "n1"),
        "n3": node("n3", message("m3", "assistant", parts=["answer"]), "n2"),
    })])
    text_scenario("13-text-citations", [conversation("c10", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=[
            "see " + chr(0xE200) + "cite" + chr(0xE202) + "turn3f12" + chr(0xE201)
            + " for details"],
            metadata={"content_references": [
                {"source_id": "turn3f12", "url": "https://example.com/doc", "title": "Doc"}]}), "n0"),
        "n2": node("n2", message("m2", "assistant", parts=["sure"]), "n1"),
    })])
    text_scenario("14-text-multimodal", [conversation("c11", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", content_type="multimodal_text", parts=[
            {"content_type": "image_asset_pointer", "asset_pointer": "file-1"},
            {"content_type": "text", "text": "look"}]), "n0"),
    })])
    text_scenario("15-text-metadata-status", [conversation("c12", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "assistant", parts=["done"],
                                 metadata={"model_slug": "gpt-4o", "is_complete": False}), "n0"),
        "n2": node("n2", message("m2", "assistant", parts=["stopped"],
                                 metadata={"finish_details": {"type": "max_tokens"}}), "n1"),
    })])
    text_scenario("16-text-no-create-time", [conversation("c13", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["hi"], create_time=None), "n0"),
    })])
    text_scenario("17-text-selection", [conversation("c14", simple_mapping()),
                                        conversation("c15", simple_mapping())],
                  selected=["c15"])
    text_scenario("18-text-unicode", [conversation("c16", {
        "n0": node("n0"),
        "n1": node("n1", message("m1", "user", parts=["привет 🙂"]), "n0"),
    }, title="Юникод")])
    text_scenario("19-text-empty-title", [conversation("c17", simple_mapping(), title="")])
    text_scenario("20-text-multiple-conversations",
                  [conversation("c18", simple_mapping()),
                   conversation("c19", simple_mapping(), update_time=1700000001.0)])

    # ------------------------------------------------------------------ zip archives
    payload = json.dumps([conversation("z1", simple_mapping())], ensure_ascii=False)
    zip_scenario("30-zip-conversations", [{"name": "conversations.json", "content": payload}])
    zip_scenario("31-zip-split-conversations", [
        {"name": "conversations-1.json",
         "content": json.dumps([conversation("z2", simple_mapping())])},
        {"name": "conversations-2.json",
         "content": json.dumps([conversation("z3", simple_mapping())])},
    ])
    zip_scenario("32-zip-unrelated-entry", [
        {"name": "chat.html", "content": "<html></html>"},
    ])
    zip_scenario("33-zip-stored-entry", [
        {"name": "conversations.json", "content": payload, "stored": "true"},
    ])
    zip_scenario("34-zip-mixed-entries", [
        {"name": "user.json", "content": "{}"},
        {"name": "conversations.json", "content": payload},
    ])
    zip_scenario("35-zip-selection", [
        {"name": "conversations.json", "content": payload},
    ], selected=["z1"])

    # ----------------------------------------------------------------------- fuzz
    for index in range(8):
        rng = random.Random(7000 + index)
        mapping: dict = {}
        node_count = rng.randint(2, 7)
        for node_index in range(node_count):
            node_id = "n%d" % node_index
            parent = ROOT if node_index == 0 else "n%d" % rng.randint(0, node_index - 1)
            if rng.random() < 0.2:
                parent = "n%d" % rng.randint(0, node_count)
            message_value = None
            if rng.random() < 0.85:
                role = rng.choice(["user", "assistant", "tool", "system"])
                content_type = rng.choice(["text", "text", "multimodal_text", "code",
                                           "execution_output", "thoughts"])
                parts = None
                text = None
                result = None
                thoughts = None
                if content_type in ("text", "multimodal_text"):
                    parts = [rng.choice(["hello", "world", {"content_type": "text",
                                                           "text": "obj"}])]
                elif content_type == "code":
                    text = "code()"
                elif content_type == "execution_output":
                    result = "output"
                elif content_type == "thoughts":
                    thoughts = [{"content": "thinking", "summary": "sum"}]
                message_value = message("m%d" % node_index, role, parts=parts,
                                        content_type=content_type, text=text, result=result,
                                        thoughts=thoughts,
                                        create_time=rng.choice([1700000000.0, 1700000000.5, None]))
            mapping[node_id] = node(node_id, message_value, parent)
        current = "n%d" % rng.randint(0, node_count) if rng.random() < 0.5 else None
        text_scenario("90-fuzz-%02d" % index,
                      [conversation("f%d" % index, mapping, current_node=current,
                                    title=rng.choice(["", "Fuzz", "Фаз"]))])

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
