#!/usr/bin/env python3
"""Deterministic scenario generator for the OpenAI Responses request module."""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")


def write(name: str, scenario: dict) -> None:
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def text_part(text: str) -> dict:
    return {"type": "text", "text": text}


def image_part(url: str) -> dict:
    return {"type": "image_url", "image_url": {"url": url}}


def user(text: str) -> dict:
    return {"role": "user", "content": [text_part(text)]}


def assistant(text: str) -> dict:
    return {"role": "assistant", "content": [text_part(text)]}


def tool_message(call_id: str, text: str) -> dict:
    return {"role": "tool", "tool_call_id": call_id, "content": [text_part(text)]}


def function_call(call_id: str, name: str, arguments: str = "{}") -> dict:
    return {"id": call_id, "type": "function", "function": {"name": name, "arguments": arguments}}


def tool_definition(name: str, description: str = "", parameters: dict | None = None) -> dict:
    # ToolFunction.parameters has no default: the wire form always carries a schema.
    function = {
        "name": name,
        "description": description,
        "parameters": parameters if parameters is not None else {
            "type": "object", "properties": {}, "required": [],
        },
    }
    return {"type": "function", "function": function}


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # ------------------------------------------------------------------- input
    write("01-input-user-text", {"op": "input", "messages": [user("hello")]})
    write("02-input-assistant-text", {"op": "input", "messages": [assistant("hi there")]})
    write("03-input-system-and-developer", {"op": "input", "messages": [
        {"role": "system", "content": [text_part("be brief")]},
        {"role": "developer", "content": [text_part("follow the rules")]},
    ]})
    write("04-input-tool-output", {"op": "input", "messages": [
        user("run it"), tool_message("call_1", "done"),
    ]})
    write("05-input-assistant-tool-calls", {"op": "input", "messages": [
        {"role": "assistant", "tool_calls": [
            function_call("call_1", "search", '{"q":"x"}'),
            function_call("call_2", "fetch"),
        ]},
    ]})
    write("06-input-image-part", {"op": "input", "messages": [
        {"role": "user", "content": [image_part("https://img.example/a.png"), text_part("what?")]},
    ]})
    write("07-input-assistant-image-part", {"op": "input", "messages": [
        {"role": "assistant", "content": [image_part("https://img.example/b.png")]},
    ]})
    write("08-input-empty-content", {"op": "input", "messages": [
        {"role": "user", "content": []},
        {"role": "user"},
    ]})
    write("09-input-empty-tool-output", {"op": "input", "messages": [
        tool_message("call_1", ""),
    ]})
    reasoning_items = [{
        "type": "reasoning",
        "id": "rs_1",
        "summary": [{"type": "summary_text", "text": "thought"}],
        "encrypted_content": "abc",
    }]
    write("10-input-replay-reasoning", {
        "op": "input", "provider": "openai", "target_model": "gpt-5.4",
        "messages": [{
            "role": "assistant",
            "response_output_items": reasoning_items,
            "response_output_item_provider": "openai",
            "response_output_item_model": "gpt-5.4",
        }],
    })
    write("11-input-replay-provider-mismatch", {
        "op": "input", "provider": "openai", "target_model": "gpt-5.4",
        "messages": [{
            "role": "assistant",
            "response_output_items": reasoning_items,
            "response_output_item_provider": "anthropic",
            "response_output_item_model": "gpt-5.4",
        }],
    })
    write("12-input-replay-model-mismatch", {
        "op": "input", "provider": "openai", "target_model": "gpt-5.4",
        "messages": [{
            "role": "assistant",
            "response_output_items": reasoning_items,
            "response_output_item_provider": "openai",
            "response_output_item_model": "gpt-4o",
        }],
    })
    write("13-input-replay-case-insensitive-model", {
        "op": "input", "provider": "openai", "target_model": "GPT-5.4",
        "messages": [{
            "role": "assistant",
            "response_output_items": reasoning_items,
            "response_output_item_provider": "openai",
            "response_output_item_model": "models/gpt-5.4",
        }],
    })
    write("14-input-replay-without-target-model", {
        "op": "input", "provider": "openai",
        "messages": [{
            "role": "assistant",
            "response_output_items": reasoning_items,
            "response_output_item_provider": "openai",
            "response_output_item_model": "gpt-5.4",
        }],
    })
    write("15-input-replay-falls-back-to-tool-calls", {
        "op": "input", "provider": "openai", "target_model": "gpt-5.4",
        "messages": [{
            "role": "assistant",
            "tool_calls": [function_call("call_9", "search")],
            "response_output_items": reasoning_items,
            "response_output_item_provider": "openai",
            "response_output_item_model": "gpt-4o",
        }],
    })
    write("16-input-reasoning-content-only", {"op": "input", "messages": [
        {"role": "assistant", "reasoning_content": "because", "content": [text_part("answer")]},
    ]})
    write("17-input-unicode", {"op": "input", "messages": [user("привет 🙂")]})

    # ------------------------------------------------------------------- tools
    write("20-tools-empty", {"op": "tools", "tools": []})
    write("21-tools-one", {"op": "tools", "tools": [
        tool_definition("search", "Search the web"),
    ]})
    write("22-tools-with-parameters", {"op": "tools", "tools": [
        tool_definition("search", "Search", {
            "type": "object",
            "properties": {"q": {"type": "string", "description": "query"}},
            "required": ["q"],
        }),
    ]})
    write("23-tools-several", {"op": "tools", "tools": [
        tool_definition("a", "first"),
        tool_definition("b", "second"),
    ]})

    # ----------------------------------------------------------------- validate
    def request(input_items, **extra):
        payload = {"model": "gpt-5.4", "input": input_items, "stream": True}
        payload.update(extra)
        return payload

    def message_item(role: str, content_type: str = "input_text", text: str = "hi") -> dict:
        return {"type": "message", "role": role,
                "content": [{"type": content_type, "text": text}]}

    write("30-validate-ok", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
    ])})
    write("31-validate-blank-model", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], model="   ")})
    write("32-validate-empty-input", {"op": "validate", "provider": "openai", "request": request([])})
    write("33-validate-max-output-tokens", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], max_output_tokens=0)})
    write("34-validate-top-p-out-of-range", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], top_p=1.5)})
    write("35-validate-unsafe-tool-name", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], tools=[{"type": "function", "name": "bad name!"}])})
    write("36-validate-hosted-web-search", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], tools=[{"type": "web_search", "name": "search"}])})
    write("37-validate-unsupported-tool-type", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user")], tools=[{"type": "custom"}])})
    write("38-validate-invalid-role", {"op": "validate", "provider": "openai", "request": request([
        {"type": "message", "role": "wizard", "content": [{"type": "input_text", "text": "x"}]},
    ])})
    write("39-validate-empty-content", {"op": "validate", "provider": "openai", "request": request([
        {"type": "message", "role": "user", "content": []},
    ])})
    write("40-validate-assistant-with-input-text", {"op": "validate", "provider": "openai",
          "request": request([message_item("assistant", "input_text")])})
    write("41-validate-user-with-output-text", {"op": "validate", "provider": "openai",
          "request": request([message_item("user", "output_text")])})
    write("42-validate-image-without-detail", {"op": "validate", "provider": "openai", "request": request([
        {"type": "message", "role": "user",
         "content": [{"type": "input_image", "image_url": "https://i.example/a.png"}]},
    ])})
    write("43-validate-unsafe-call-id", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "function_call", "call_id": "bad id!", "name": "f", "arguments": "{}"},
    ])})
    write("44-validate-duplicate-call-id", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "function_call", "call_id": "call_1", "name": "f", "arguments": "{}"},
        {"type": "function_call", "call_id": "call_1", "name": "f", "arguments": "{}"},
    ])})
    write("45-validate-missing-output", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "function_call", "call_id": "call_1", "name": "f", "arguments": "{}"},
    ])})
    write("46-validate-output-without-call", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "function_call_output", "call_id": "call_1", "output": "done"},
    ])})
    write("47-validate-not-ending-in-user", {"op": "validate", "provider": "openai", "request": request([
        message_item("assistant", "output_text"),
    ])})
    write("48-validate-reasoning-blank-id", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "reasoning", "id": "  "},
    ])})
    write("49-validate-opaque-item-extra-fields", {"op": "validate", "provider": "openai",
          "request": request([
              message_item("user"),
              {"type": "web_search_call", "id": "ws_1", "status": "done"},
          ])})
    write("50-validate-message-extra-fields", {"op": "validate", "provider": "openai", "request": request([
        {"type": "message", "role": "user", "content": [{"type": "input_text", "text": "x"}],
         "id": "msg_1"},
    ])})
    write("51-validate-multiple-violations", {"op": "validate", "provider": "openai", "request": request(
        [message_item("assistant", "input_text")], model="", max_output_tokens=-1, top_p=2.0)})
    write("52-validate-arguments-not-object", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "function_call", "call_id": "call_1", "name": "f", "arguments": "not json"},
    ])})
    write("53-validate-unsupported-input-item", {"op": "validate", "provider": "openai", "request": request([
        message_item("user"),
        {"type": "mystery"},
    ])})
    write("54-validate-duplicate-violations", {"op": "validate", "provider": "openai", "request": request(
        [message_item("user"), message_item("user")], model="")})

    # --------------------------------------------------------------------- fuzz
    for index in range(8):
        rng = random.Random(11000 + index)
        items = []
        for _ in range(rng.randint(1, 4)):
            kind = rng.choice(["message", "function_call", "function_call_output", "reasoning",
                               "opaque"])
            if kind == "message":
                role = rng.choice(["user", "assistant", "system", "developer", "wizard"])
                content_type = rng.choice(["input_text", "output_text", "input_image"])
                content = []
                if rng.random() < 0.85:
                    content.append({"type": content_type, "text": "text"})
                if content_type == "input_image" and rng.random() < 0.5:
                    content[0] = {"type": "input_image", "image_url": "https://i.example/a.png",
                                  "detail": "auto"}
                items.append({"type": "message", "role": role, "content": content})
            elif kind == "function_call":
                items.append({"type": "function_call",
                              "call_id": rng.choice(["call_1", "bad id", "call_2"]),
                              "name": rng.choice(["f", "bad name", "g"]),
                              "arguments": rng.choice(["{}", "not json", "[]"])})
            elif kind == "function_call_output":
                items.append({"type": "function_call_output",
                              "call_id": rng.choice(["call_1", "call_2"]),
                              "output": rng.choice(["done", ""])})
            elif kind == "reasoning":
                items.append({"type": "reasoning", "id": rng.choice(["rs_1", "  "])})
            else:
                items.append({"type": "opaque", "id": rng.choice(["op_1", ""])})
        payload = {"model": rng.choice(["gpt-5.4", "", "  "]), "input": items, "stream": True}
        if rng.random() < 0.3:
            payload["top_p"] = rng.choice([0.0, 0.5, 1.0, 1.5, -0.5])
        if rng.random() < 0.3:
            payload["max_output_tokens"] = rng.choice([1, 0, -5, 100])
        if rng.random() < 0.3:
            payload["tools"] = [{"type": rng.choice(["function", "web_search", "custom"]),
                                 "name": rng.choice(["f", "bad name"])}]
        write("90-fuzz-%02d" % index, {"op": "validate", "provider": "openai", "request": payload})

    print("Wrote %d scenarios to %s" % (len(os.listdir(OUT)), OUT))


if __name__ == "__main__":
    main()
