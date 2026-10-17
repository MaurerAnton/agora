#!/usr/bin/env python3
"""Generates the scenario corpus for the Ollama golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

TOOL_WEATHER = {
    "type": "function",
    "function": {
        "name": "get_weather",
        "description": "Get current weather for a location",
        "parameters": {
            "type": "object",
            "properties": {"location": {"type": "string", "description": "City and state"}},
            "required": ["location"],
        },
    },
}

ALL = []


def scenario(name, mode, **kw):
    out = {"name": name, "mode": mode}
    out.update(kw)
    ALL.append(out)


def factory(name, messages, config=None, system=None):
    request_input = {"messages": messages}
    if system is not None:
        request_input["systemPrompt"] = system
    scenario(name, "factory", config=config or {}, requestInput=request_input)


def validate(name, request, config=None):
    out = {"name": name, "mode": "validate", "request": request}
    if config is not None:
        out["config"] = config
    ALL.append(out)


def serialized(name, body, strings=("model",), arrays=("messages",)):
    scenario(name, "serialized", serializedBody=body,
             serializedRequiredStringFields=list(strings),
             serializedRequiredArrayFields=list(arrays))


def stream(name, ops):
    scenario(name, "stream", ops=ops)


def msg(mid, participant="USER", text="", images=None, model_name=None, segments=None,
        tool_call=None):
    out = {"id": mid, "participant": participant, "text": text}
    if images:
        out["images"] = images
    if model_name is not None:
        out["modelName"] = model_name
    if segments is not None:
        out["segments"] = segments
    if tool_call is not None:
        out["toolCall"] = tool_call
    return out


def seg(stype, content="", tool_name=None, tool_args=None, tool_result=None,
        tool_call_id=None, signature=None, signature_provider=None):
    out = {"type": stype, "content": content}
    if tool_name is not None:
        out["toolName"] = tool_name
    if tool_args is not None:
        out["toolArgs"] = tool_args
    if tool_result is not None:
        out["toolResult"] = tool_result
    if tool_call_id is not None:
        out["toolCallId"] = tool_call_id
    if signature is not None:
        out["signature"] = signature
    if signature_provider is not None:
        out["signatureProvider"] = signature_provider
    return out


def tc(tool_name, arguments, result, tool_call_id=None):
    out = {"toolName": tool_name, "arguments": arguments, "result": result}
    if tool_call_id is not None:
        out["toolCallId"] = tool_call_id
    return out


USER_TEXT = "Hello"


# ---- factory: system / options / think -----------------------------------------------------
factory("ollama_basic_text", [msg("user_1", text="Hello")],
        system="You are helpful.")
factory("ollama_system_blank_dropped", [msg("user_1", text="Hello")],
        system="   ")
factory("ollama_options_all", [msg("user_1", text="Hi")],
        config={"temperature": 0.7, "topP": 0.9, "maxTokens": 1024})
factory("ollama_options_empty", [msg("user_1", text="Hi")])
factory("ollama_nan_temperature_rejected", [msg("user_1", text="Hi")],
        config={"temperature": "NaN"})
factory("ollama_think_default_true", [msg("user_1", text="Hi")],
        config={"modelId": "llama3.2:3b"})
factory("ollama_think_effort_level", [msg("user_1", text="Hi")],
        config={"modelId": "gpt-oss:8b", "thinkingLevel": "high"})
factory("ollama_think_disabled", [msg("user_1", text="Hi")],
        config={"modelId": "gpt-oss:8b", "thinkingEnabled": False})

# ---- factory: images ---------------------------------------------------------------------------
factory("ollama_images_registered", [msg("user_1", text="look", images=["files/a.png"])])
factory("ollama_images_assistant_dropped",
        [msg("user_1", text="draw"),
         msg("model_1", participant="MODEL", text="done", images=["files/a.png"])])
factory("ollama_images_disabled", [msg("user_1", text="look", images=["files/a.png"])],
        config={"includeImages": False})
factory("ollama_missing_image_filtered", [msg("user_1", text="look", images=["files/missing.png"])])
factory("ollama_whitespace_user_rejected", [msg("user_1", text="   ")])

# ---- factory: tool rounds ------------------------------------------------------------------------
factory("ollama_tool_round_segments",
        [msg("user_1", text="Weather?"),
         msg("tool_1", participant="MODEL", model_name="Ollama:llama3.2:3b", segments=[
             seg("thought", content="Looking it up."),
             seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
                 tool_call_id="call_1"),
         ]),
         msg("result_1", segments=[
             seg("tool", tool_name="get_weather", tool_result="Sunny", tool_call_id="call_1"),
         ]),
         msg("user_2", text="Thanks!")],
        config={"tools": [TOOL_WEATHER]})
factory("ollama_tool_round_legacy",
        [msg("user_1", text="Weather?"),
         msg("tool_1", participant="MODEL",
             tool_call=tc("get_weather", '{"location": "Oslo"}', "", "call_l")),
         msg("result_1", tool_call=tc("get_weather", '{"location": "Oslo"}', "Sunny")),
         msg("user_2", text="Thanks!")],
        config={"tools": [TOOL_WEATHER]})
factory("ollama_empty_tool_row_contributes_nothing",
        [msg("user_1", text="Hi"),
         msg("tool_1", participant="MODEL"),
         msg("user_2", text="Thanks!")])
factory("ollama_thought_segment_empty_thinking",
        [msg("user_1", text="run it"),
         msg("model_1", participant="MODEL", segments=[
             seg("thought", content=""),
             seg("tool", tool_name="get_weather", tool_args="{}", tool_call_id="call_1"),
         ]),
         msg("result_1", segments=[
             seg("tool", tool_name="get_weather", tool_result="ok", tool_call_id="call_1"),
         ]),
         msg("user_2", text="Thanks!")])
factory("ollama_tools_wire", [msg("user_1", text="Go")],
        config={"tools": [TOOL_WEATHER]})
factory("ollama_empty_messages_rejected", [])

# ---- validate: passing shapes --------------------------------------------------------------------
validate("validate_ok_tool_round", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "user", "content": "Weather?"},
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": {"location": "Oslo"}}},
        ]},
        {"role": "tool", "content": "Sunny", "tool_name": "get_weather"},
        {"role": "user", "content": "Thanks"},
    ],
})
validate("validate_ok_think_strings", {
    "model": "gpt-oss:8b",
    "think": "low",
    "messages": [{"role": "user", "content": "hi"}],
})
validate("validate_ok_think_boolean", {
    "model": "llama3.2:3b",
    "think": True,
    "messages": [{"role": "user", "content": "hi"}],
})

# ---- validate: structural violations ------------------------------------------------------------
validate("validate_invalid_role", {
    "model": "llama3.2:3b",
    "messages": [{"role": "moderator", "content": "hi"}],
})
validate("validate_system_after_conversation", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "user", "content": "hi"},
        {"role": "system", "content": "late system"},
    ],
})
validate("validate_system_with_tool_fields", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "system", "content": "sys", "tool_name": "get_weather"},
    ],
})
validate("validate_user_interrupts_pending", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": {}}},
        ]},
        {"role": "user", "content": "interrupt"},
    ],
})
validate("validate_user_empty_content", {
    "model": "llama3.2:3b",
    "messages": [{"role": "user", "content": "   "}],
})
validate("validate_user_carries_tool_fields", {
    "model": "llama3.2:3b",
    "messages": [{"role": "user", "content": "hi",
                  "tool_calls": [{"id": "call_1", "type": "function",
                                  "function": {"name": "x", "arguments": {}}}]}],
})
validate("validate_assistant_tool_name", {
    "model": "llama3.2:3b",
    "messages": [{"role": "assistant", "content": "hi", "tool_name": "get_weather"}],
})
validate("validate_assistant_content_empty", {
    "model": "llama3.2:3b",
    "messages": [{"role": "assistant", "content": ""}],
})
validate("validate_assistant_starts_before_results", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "one", "arguments": {}}},
        ]},
        {"role": "assistant", "content": "more"},
        {"role": "tool", "content": "result", "tool_name": "one"},
    ],
})
validate("validate_call_type_not_function", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "tool",
             "function": {"name": "get_weather", "arguments": {}}},
        ]},
        {"role": "tool", "content": "Sunny", "tool_name": "get_weather"},
    ],
})
validate("validate_call_name_unsafe", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "bad name", "arguments": {}}},
        ]},
        {"role": "tool", "content": "Sunny", "tool_name": "bad name"},
    ],
})
validate("validate_call_arguments_not_object", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": "not an object"}},
        ]},
        {"role": "tool", "content": "Sunny", "tool_name": "get_weather"},
    ],
})
validate("validate_tool_name_unsafe", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "tool", "content": "Sunny", "tool_name": "bad name"},
    ],
})
validate("validate_tool_without_pending", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "user", "content": "hi"},
        {"role": "tool", "content": "Sunny", "tool_name": "get_weather"},
    ],
})
validate("validate_tool_result_carries_calls", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": {}}},
        ]},
        {"role": "tool", "content": "Sunny", "tool_name": "get_weather",
         "tool_calls": [{"id": "call_1", "type": "function",
                         "function": {"name": "get_weather", "arguments": {}}}]},
    ],
})
validate("validate_missing_results", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "user", "content": "hi"},
        {"role": "assistant", "content": "", "tool_calls": [
            {"id": "call_1", "type": "function",
             "function": {"name": "get_weather", "arguments": {}}},
        ]},
    ],
})
validate("validate_history_starts_with_assistant", {
    "model": "llama3.2:3b",
    "messages": [{"role": "assistant", "content": "I start"}],
})
validate("validate_history_ends_with_assistant", {
    "model": "llama3.2:3b",
    "messages": [
        {"role": "user", "content": "hi"},
        {"role": "assistant", "content": "answer"},
    ],
})
validate("validate_think_object", {
    "model": "llama3.2:3b",
    "think": {},
    "messages": [{"role": "user", "content": "hi"}],
})
validate("validate_think_number", {
    "model": "llama3.2:3b",
    "think": 42,
    "messages": [{"role": "user", "content": "hi"}],
})
validate("validate_think_invalid_string", {
    "model": "llama3.2:3b",
    "think": "ultra",
    "messages": [{"role": "user", "content": "hi"}],
})
validate("validate_model_blank", {
    "model": "   ",
    "messages": [{"role": "user", "content": "hi"}],
})
validate("validate_empty_messages", {"model": "llama3.2:3b", "messages": []})
validate("validate_tools_unsafe_and_duplicate", {
    "model": "llama3.2:3b",
    "messages": [{"role": "user", "content": "hi"}],
    "tools": [
        {"type": "function", "function": {
            "name": "bad name", "description": "x",
            "parameters": {"type": "object", "properties": {}}}},
        {"type": "function", "function": {
            "name": "lookup", "description": "x",
            "parameters": {"type": "object", "properties": {}}}},
        {"type": "function", "function": {
            "name": "lookup", "description": "x",
            "parameters": {"type": "object", "properties": {}}}},
    ],
})

# ---- validate: kotlinx decode behaviour ------------------------------------------------------------
validate("validate_decode_model_missing", {"messages": []})
validate("validate_decode_messages_missing", {"model": "llama3.2:3b"})
validate("validate_decode_role_missing", {
    "model": "llama3.2:3b",
    "messages": [{"content": "hi"}],
})
validate("validate_decode_think_number_lexeme", {
    "model": "llama3.2:3b",
    "think": 1.5,
    "messages": [{"role": "user", "content": "hi"}],
})

# ---- serialized-body gate ----------------------------------------------------------------------------
serialized("serialized_ok", '{"model":"llama3.2:3b","messages":[{"role":"user"}]}')
serialized("serialized_missing_model", '{"messages":[{"role":"user"}]}')
serialized("serialized_empty_messages", '{"model":"llama3.2:3b","messages":[]}')
serialized("serialized_not_object", '[]')


# ---- stream: NDJSON loop -----------------------------------------------------------------------------


def data(payload):
    return "L " + json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


END = "E"

stream("stream_text_done", [
    data({"message": {"role": "assistant", "content": "Hel"}, "done": False}),
    data({"message": {"role": "assistant", "content": "lo"}, "done": True,
          "done_reason": "stop", "prompt_eval_count": 10, "eval_count": 5}),
    data({"message": {"role": "assistant", "content": "late"}}),
    END,
])
stream("stream_thinking_field", [
    data({"message": {"role": "assistant", "thinking": "Let me think."}}),
    data({"message": {"role": "assistant", "content": "answer"}, "done": True,
          "done_reason": "stop", "prompt_eval_count": 3, "eval_count": 4}),
    END,
])
stream("stream_tool_batch_single", [
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [{"id": "call_1", "type": "function",
                                      "function": {"name": "get_weather",
                                                   "arguments": {"location": "Oslo"}}}]}}),
    data({"message": {"role": "assistant", "content": "checking"}, "done": True,
          "done_reason": "stop"}),
    END,
])
stream("stream_tool_batch_multi", [
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_1", "type": "function",
                           "function": {"name": "one", "arguments": {"a": 1}}},
                          {"id": "call_2", "type": "function",
                           "function": {"name": "two", "arguments": {"b": 2}}},
                      ]}}),
    data({"done": True, "done_reason": "stop"}),
    END,
])
stream("stream_tool_batch_damaged", [
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_1", "type": "function",
                           "function": {"name": "good", "arguments": {"a": 1}}},
                          {"id": "call_2", "type": "function",
                           "function": {"name": "bad name", "arguments": {}}},
                          {"id": "call_3", "type": "function",
                           "function": {"name": "weird", "arguments": "not json"}},
                      ]}}),
    data({"done": True, "done_reason": "stop"}),
    END,
])
stream("stream_tool_args_variants", [
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_1",
                           "function": {"name": "from_string", "arguments": "{\"q\":1}"}},
                      ]}}),
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_2",
                           "function": {"name": "from_number", "arguments": 42}},
                      ]}}),
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_3", "function": {"name": "no_function_args"}},
                      ]}}),
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "", "type": "function",
                           "function": {"name": "blank_id", "arguments": {}}},
                      ]}}),
    data({"done": True, "done_reason": "stop"}),
    END,
])
stream("stream_tool_duplicate_id_in_batch", [
    data({"message": {"role": "assistant", "content": "",
                      "tool_calls": [
                          {"id": "call_1", "function": {"name": "one", "arguments": {}}},
                          {"id": "call_1", "function": {"name": "two", "arguments": {}}},
                      ]}}),
    data({"done": True, "done_reason": "stop"}),
    END,
])
stream("stream_error_field", [
    data({"error": "model not loaded"}),
    data({"message": {"role": "assistant", "content": "late"}}),
    END,
])
stream("stream_error_blank_ignored", [
    data({"error": "   "}),
    data({"message": {"role": "assistant", "content": "kept"}, "done": True,
          "done_reason": "STOP"}),
    END,
])
stream("stream_done_reason_normalized", [
    data({"message": {"role": "assistant", "content": "hi"}, "done": True,
          "done_reason": "  Custom Stop  "}),
    END,
])
stream("stream_done_reason_blank", [
    data({"done": True, "done_reason": "   "}),
    END,
])
stream("stream_break_on_done", [
    data({"message": {"role": "assistant", "content": "a"}, "done": True,
          "done_reason": "stop"}),
    data({"message": {"role": "assistant", "content": "after"}}),
    END,
])
stream("stream_decode_failure_take512", [
    "L " + ("x" * 600),
    data({"message": {"role": "assistant", "content": "late"}}),
    END,
])
stream("stream_usage_coercion", [
    data({"message": {"role": "assistant", "content": "hi"}, "done": True,
          "prompt_eval_count": -3, "eval_count": 7}),
    END,
])
stream("stream_retryable_without_done", [
    data({"message": {"role": "assistant", "content": "partial"}}),
    END,
])


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(item, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
