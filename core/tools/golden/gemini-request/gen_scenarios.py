#!/usr/bin/env python3
"""Generates the scenario corpus for the Gemini request golden stand.

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

EXTERNAL_TOOL = {
    "type": "function",
    "function": {
        "name": "mcp_tool",
        "description": "External schema",
        "parameters": {
            "type": "object",
            "properties": {},
            "schema": {"type": "object", "additionalProperties": {"type": "string"},
                       "x-external": True},
        },
    },
}

CODE_TOOL = {
    "type": "function",
    "function": {
        "name": "code_execution",
        "description": "Run code",
        "parameters": {"type": "object", "properties": {}},
    },
}


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


def tc(tool_name, arguments, result, tool_call_id=None, signature=None):
    out = {"toolName": tool_name, "arguments": arguments, "result": result}
    if tool_call_id is not None:
        out["toolCallId"] = tool_call_id
    if signature is not None:
        out["signature"] = signature
    return out


def factory(name, messages, config=None, system=None):
    out = {"name": name, "providerName": "Google", "mode": "factory"}
    out["config"] = config or {}
    request_input = {"messages": messages}
    if system is not None:
        request_input["systemPrompt"] = system
    out["requestInput"] = request_input
    return out


def validate(name, request, config=None):
    out = {"name": name, "providerName": "Google", "mode": "validate", "request": request}
    if config is not None:
        out["config"] = config
    return out


def serialized(name, body, strings=(), arrays=("contents",)):
    return {
        "name": name,
        "providerName": "Google",
        "mode": "serialized",
        "serializedBody": body,
        "serializedRequiredStringFields": list(strings),
        "serializedRequiredArrayFields": list(arrays),
    }


def part(ptype, **kw):
    out = {"type": ptype}
    out.update(kw)
    return out


USER_TEXT = [part("text", text="Hello")]

ALL = []

# ---- factory: basic shapes -------------------------------------------------------------------------
ALL.append(factory("gemini_basic_text", [msg("user_1", text="Hello")]))
ALL.append(factory(
    "gemini_system_prompt",
    [msg("user_1", text="Hello")],
    system="You are helpful.",
))
ALL.append(factory(
    "gemini_system_blank_dropped",
    [msg("user_1", text="Hello")],
    system="   ",
))
ALL.append(factory(
    "gemini_images",
    [msg("user_1", text="look", images=["files/a.png"])],
))
ALL.append(factory(
    "gemini_images_disabled",
    [msg("user_1", text="look", images=["files/a.png"])],
    config={"includeImages": False},
))
ALL.append(factory(
    "gemini_assistant_images_skipped",
    [msg("user_1", text="draw"),
     msg("model_1", participant="MODEL", text="done", images=["files/a.png"])],
))
ALL.append(factory(
    "gemini_empty_user_attachment",
    [msg("user_1", text="")],
))
ALL.append(factory(
    "gemini_missing_image_keeps_text",
    [msg("user_1", text="look", images=["files/missing.png"])],
))

# ---- factory: code-execution history preservation ---------------------------------------------------
CODE_SEGMENTS = [
    seg("answer", content="Running the check."),
    seg("tool", tool_name="code_execution",
        tool_args='{"language": "python", "code": "print(1)"}',
        tool_result='{"outcome": "OUTCOME_OK", "output": "1"}'),
]
ALL.append(factory(
    "gemini_code_execution_history",
    [msg("user_1", text="run it"),
     msg("model_1", participant="MODEL", model_name="Google:gemini-2.5-pro",
         segments=CODE_SEGMENTS),
     msg("user_2", text="thanks")],
))
ALL.append(factory(
    "gemini_code_execution_malformed_json",
    [msg("user_1", text="run it"),
     msg("model_1", participant="MODEL", model_name="Google:gemini-2.5-pro",
         segments=[
             seg("answer", content="Running the check."),
             seg("tool", tool_name="code_execution", tool_args="not json",
                 tool_result="also not json"),
         ]),
     msg("user_2", text="thanks")],
))
ALL.append(factory(
    "gemini_model_no_answer_segment",
    [msg("user_1", text="run it"),
     msg("model_1", participant="MODEL", text="plain answer"),
     msg("user_2", text="thanks")],
))

# ---- factory: tool rounds --------------------------------------------------------------------------
ALL.append(factory(
    "gemini_tool_round",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         segments=[seg("tool", tool_name="get_weather",
                       tool_args='{"location": "Oslo"}', tool_call_id="call_1")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_tool_round_legacy",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         tool_call=tc("get_weather", '{"location": "Oslo"}', "", "call_l")),
     msg("result_1", tool_call=tc("get_weather", '{"location": "Oslo"}', "sunny")),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_tool_result_raw_string",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         segments=[seg("tool", tool_name="get_weather",
                       tool_args='{"location": "Oslo"}', tool_call_id="call_1")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result="plain text result", tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_tool_round_args_not_object",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         segments=[seg("tool", tool_name="get_weather",
                       tool_args='[1,2]', tool_call_id="call_1")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_empty_tool_row_archived",
    [msg("user_1", text="Hi"),
     msg("tool_1", participant="MODEL"),
     msg("user_2", text="Thanks!")],
))

# ---- factory: signature compatibility (adaptToolRounds) -----------------------------------------------
SIGNED_TOOL_SEGMENTS = [
    seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
        tool_call_id="call_1", signature="sig-abc", signature_provider="Google"),
]
ALL.append(factory(
    "gemini_signed_round_replayed",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Google:gemini-2.5-pro",
         segments=SIGNED_TOOL_SEGMENTS),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_signed_round_foreign_provider_archived",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Google:gemini-2.5-pro",
         segments=[
             seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
                 tool_call_id="call_1", signature="sig-abc",
                 signature_provider="Vertex")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_unsigned_round_gemini3_archived",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Google:gemini-3-pro",
         segments=[seg("tool", tool_name="get_weather",
                       tool_args='{"location": "Oslo"}', tool_call_id="call_1")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"modelId": "gemini-3-pro", "tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_unsigned_round_gemini3_ok_kept",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Google:gemini-2.5-pro",
         segments=[seg("tool", tool_name="get_weather",
                       tool_args='{"location": "Oslo"}', tool_call_id="call_1")]),
     msg("result_1",
         segments=[seg("tool", tool_name="get_weather",
                       tool_result='{"temp": 12}', tool_call_id="call_1")]),
     msg("user_2", text="Thanks!")],
    config={"modelId": "gemini-2.5-pro", "tools": [TOOL_WEATHER]},
))

# ---- factory: tools and toolConfig ----------------------------------------------------------------------
ALL.append(factory(
    "gemini_builtins_and_functions",
    [msg("user_1", text="search it")],
    config={"codeExecutionEnabled": True, "googleSearchEnabled": True,
            "tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_builtins_only_no_tool_config",
    [msg("user_1", text="search it")],
    config={"googleSearchEnabled": True},
))
ALL.append(factory(
    "gemini_functions_only_no_tool_config",
    [msg("user_1", text="search it")],
    config={"tools": [TOOL_WEATHER]},
))
ALL.append(factory(
    "gemini_external_schema_to_parameters_json_schema",
    [msg("user_1", text="search it")],
    config={"tools": [EXTERNAL_TOOL]},
))

# ---- factory: thinkingConfig forms -----------------------------------------------------------------------
ALL.append(factory(
    "gemini_thinking_disabled",
    [msg("user_1", text="Hi")],
    config={"thinkingEnabled": False},
))
ALL.append(factory(
    "gemini_thinking_budget_form",
    [msg("user_1", text="Hi")],
    config={"thinkingBudgetEnabled": True, "thinkingBudgetTokens": 8192},
))
ALL.append(factory(
    "gemini_thinking_level_form",
    [msg("user_1", text="Hi")],
    config={"thinkingLevel": "low"},
))
ALL.append(factory(
    "gemini_thinking_level_alias",
    [msg("user_1", text="Hi")],
    config={"thinkingLevel": "medium"},
))

# ---- factory: generationConfig fields --------------------------------------------------------------------
ALL.append(factory(
    "gemini_sampling_fields",
    [msg("user_1", text="Hi")],
    config={"temperature": 0.7, "maxTokens": 1024, "topP": 0.9,
            "frequencyPenalty": 0.1, "presencePenalty": 0.2},
))
ALL.append(factory(
    "gemini_nan_temperature_rejected",
    [msg("user_1", text="Hi")],
    config={"temperature": "NaN"},
))
ALL.append(factory(
    "gemini_model_name_only_affects_validation",
    [msg("user_1", text="Hi")],
    config={"modelId": "models/gemini-3-pro", "thinkingLevel": "low"},
))
ALL.append(factory(
    "gemini_model_blank_rejected",
    [msg("user_1", text="Hi")],
    config={"modelId": "models/"},
))
ALL.append(factory(
    "gemini_empty_contents_rejected",
    [],
))

# ---- validate: passing shapes ---------------------------------------------------------------------------
ALL.append(validate("validate_ok_minimal", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_ok_function_round", {
    "contents": [
        {"role": "user", "parts": USER_TEXT},
        {"role": "model", "parts": [part("functionCall", functionCall={
            "id": "call_a", "name": "lookup", "args": {"q": 1}})]},
        {"role": "user", "parts": [part("functionResponse", functionResponse={
            "id": "call_a", "name": "lookup", "response": {"ok": True}})]},
    ],
}))
ALL.append(validate("validate_ok_gemini3_signed", {
    "contents": [
        {"role": "user", "parts": USER_TEXT},
        {"role": "model", "parts": [part(
            "functionCall", thoughtSignature="sig",
            functionCall={"id": "call_a", "name": "lookup", "args": {}})]},
        {"role": "user", "parts": [part("functionResponse", functionResponse={
            "id": "call_a", "name": "lookup", "response": {"ok": True}})]},
    ],
}, config={"modelId": "gemini-3-pro"}))

# ---- validate: structural violations ---------------------------------------------------------------------
ALL.append(validate("validate_repeats_role", {
    "contents": [
        {"role": "user", "parts": USER_TEXT},
        {"role": "user", "parts": USER_TEXT},
    ],
}))
ALL.append(validate("validate_invalid_role", {
    "contents": [{"role": "system", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_response_after_other_type", {
    "contents": [{"role": "user", "parts": [
        part("text", text="hi"),
        part("functionResponse", functionResponse={"id": "call_a", "name": "x",
                                                   "response": {}}),
    ]}],
}))
ALL.append(validate("validate_result_id_mismatch", {
    "contents": [
        {"role": "model", "parts": [part("functionCall", functionCall={
            "id": "call_a", "name": "lookup", "args": {}})]},
        {"role": "user", "parts": [part("functionResponse", functionResponse={
            "id": "call_b", "name": "lookup", "response": {}})]},
    ],
}))
ALL.append(validate("validate_response_name_mismatch", {
    "contents": [
        {"role": "model", "parts": [part("functionCall", functionCall={
            "id": "call_a", "name": "lookup", "args": {}})]},
        {"role": "user", "parts": [part("functionResponse", functionResponse={
            "id": "call_a", "name": "other", "response": {}})]},
    ],
}))
ALL.append(validate("validate_orphan_function_response", {
    "contents": [{"role": "user", "parts": [
        part("functionResponse", functionResponse={"id": "call_x", "name": "x",
                                                   "response": {}}),
    ]}],
}))
ALL.append(validate("validate_missing_responses", {
    "contents": [
        {"role": "model", "parts": [part("functionCall", functionCall={
            "id": "call_a", "name": "lookup", "args": {}})]},
        {"role": "user", "parts": USER_TEXT},
    ],
}))
ALL.append(validate("validate_two_payloads_in_part", {
    "contents": [{"role": "user", "parts": [
        {"type": "text", "text": "hi", "functionCall": {"id": "c", "name": "n", "args": {}}},
    ]}],
}))
ALL.append(validate("validate_blank_text_part", {
    "contents": [{"role": "user", "parts": [part("text", text="  ")]}],
}))
ALL.append(validate("validate_inline_data_on_model", {
    "contents": [{"role": "model", "parts": [part(
        "inlineData", inlineData={"mimeType": "image/png", "data": "AAAA"})]}],
}))
ALL.append(validate("validate_executable_code_on_user", {
    "contents": [{"role": "user", "parts": [part(
        "executableCode", executableCode={"language": "python", "code": "print(1)"})]}],
}))
ALL.append(validate("validate_function_call_on_user", {
    "contents": [{"role": "user", "parts": [part("functionCall", functionCall={
        "id": "call_a", "name": "lookup", "args": {}})]}],
}))
ALL.append(validate("validate_function_call_bad_id", {
    "contents": [{"role": "model", "parts": [part("functionCall", functionCall={
        "id": "bad id!", "name": "lookup", "args": {}})]}],
}))
ALL.append(validate("validate_gemini3_function_call_unsigned", {
    "contents": [
        {"role": "model", "parts": [part("functionCall", functionCall={
            "id": "call_a", "name": "lookup", "args": {}})]},
        {"role": "user", "parts": [part("functionResponse", functionResponse={
            "id": "call_a", "name": "lookup", "response": {}})]},
    ],
}, config={"modelId": "gemini-3-pro"}))
ALL.append(validate("validate_signature_without_function_call", {
    "contents": [{"role": "model", "parts": [
        part("text", text="hi", thoughtSignature="sig"),
    ]}],
}))
ALL.append(validate("validate_start_end_roles", {
    "contents": [{"role": "model", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_empty_parts", {
    "contents": [{"role": "user", "parts": []}],
}))

# ---- validate: system instruction / tools / generationConfig ------------------------------------------
ALL.append(validate("validate_system_instruction_role", {
    "systemInstruction": {"role": "user", "parts": USER_TEXT},
    "contents": [{"role": "user", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_system_instruction_blank", {
    "systemInstruction": {"parts": [part("text", text="  ")]},
    "contents": [{"role": "user", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_system_instruction_payload", {
    "systemInstruction": {"parts": [part("text", text="ok",
                                         thoughtSignature="sig")]},
    "contents": [{"role": "user", "parts": USER_TEXT}],
}))
ALL.append(validate("validate_tools_two_types", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "tools": [
        {"code_execution": {}, "google_search": {}},
        {"function_declarations": [{"name": "lookup", "description": "x",
                                    "parameters": {"type": "object"}}]},
    ],
}))
ALL.append(validate("validate_tools_unsafe_and_duplicate", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "tools": [
        {"function_declarations": [
            {"name": "bad name", "description": "x", "parameters": {"type": "object"}},
            {"name": "lookup", "description": "x", "parameters": {"type": "object"}},
            {"name": "lookup", "description": "x", "parameters": {"type": "object"}},
        ]},
    ],
}))
ALL.append(validate("validate_tools_both_schema_forms", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "tools": [
        {"function_declarations": [{"name": "lookup", "description": "x",
                                    "parameters": {"type": "object"},
                                    "parameters_json_schema": {"type": "object"}}]},
    ],
}))
ALL.append(validate("validate_tools_not_object_schema", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "tools": [
        {"function_declarations": [{"name": "lookup", "description": "x",
                                    "parameters": {"type": "array"}}]},
    ],
}))
ALL.append(validate("validate_generation_config_gates", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "generationConfig": {
        "maxOutputTokens": -5,
        "topP": 1.5,
        "thinkingConfig": {"includeThoughts": True, "thinkingBudget": -1,
                           "thinkingLevel": "  "},
    },
}))
ALL.append(validate("validate_model_blank", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
}, config={"modelId": "models/"}))
ALL.append(validate("validate_empty_contents", {"contents": []}))

# ---- validate: kotlinx decode behaviour (class names are part of the traces) ------------------------------
ALL.append(validate("validate_decode_contents_missing", {
    "role": "user",
}))
ALL.append(validate("validate_decode_contents_null", {
    "contents": None,
}))
ALL.append(validate("validate_decode_part_payload_wrong_kind", {
    "contents": [{"role": "user", "parts": [{"type": "text", "text": 7}]}],
}))
ALL.append(validate("validate_decode_function_call_id_int", {
    "contents": [{"role": "model", "parts": [part("functionCall", functionCall={
        "id": 7, "name": "lookup", "args": {}})]}],
}))
ALL.append(validate("validate_decode_unknown_keys", {
    "contents": [{"role": "user", "parts": [{"type": "text", "text": "hi",
                                             "future_part": True}]}],
    "future_field": [1, 2],
}))
ALL.append(validate("validate_decode_max_tokens_quoted", {
    "contents": [{"role": "user", "parts": USER_TEXT}],
    "generationConfig": {"maxOutputTokens": "5000"},
}))

# ---- serialized-body gate ---------------------------------------------------------------------------------
ALL.append(serialized("serialized_ok",
                      '{"contents":[{"role":"user","parts":[{"text":"hi"}]}]}'))
ALL.append(serialized("serialized_missing_contents", '{"other": 1}'))
ALL.append(serialized("serialized_empty_contents", '{"contents":[]}'))
ALL.append(serialized("serialized_not_object", '[]'))


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for scenario in ALL:
        name = scenario["name"]
        path = os.path.join(SCENARIOS_DIR, name + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(scenario, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
