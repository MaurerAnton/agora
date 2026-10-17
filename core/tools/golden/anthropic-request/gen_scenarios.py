#!/usr/bin/env python3
"""Generates the scenario corpus for the Anthropic request golden stand.

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


def factory(name, messages, config=None, system=None):
    out = {"name": name, "providerName": "Anthropic", "mode": "factory"}
    out["config"] = config or {}
    request_input = {"messages": messages}
    if system is not None:
        request_input["systemPrompt"] = system
    out["requestInput"] = request_input
    return out


def validate(name, request, provider="Anthropic"):
    return {"name": name, "providerName": provider, "mode": "validate", "request": request}


def serialized(name, body, strings=("model",), arrays=("messages",)):
    return {
        "name": name,
        "providerName": "Anthropic",
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

# ---- factory: thinking / effort / sampling interplay --------------------------------------------
ALL.append(factory(
    "factory_thinking_adaptive",
    [msg("user_1", text="Think about this.")],
    config={"modelId": "claude-opus-4-6", "temperature": 0.7, "topP": 0.97},
))
ALL.append(factory(
    "factory_thinking_disabled_drops_sampling",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-opus-4-6", "thinkingEnabled": False, "temperature": 0.7},
))
ALL.append(factory(
    "factory_thinking_budget",
    [msg("user_1", text="Deep dive")],
    config={
        "modelId": "claude-3-7-sonnet",
        "thinkingBudgetEnabled": True,
        "thinkingBudgetTokens": 8192,
        "temperature": 0.7,
    },
))
ALL.append(factory(
    "factory_budget_capped_by_ceiling",
    [msg("user_1", text="Deep dive")],
    config={
        "modelId": "claude-3-7-sonnet",
        "thinkingBudgetEnabled": True,
        "thinkingBudgetTokens": 60000,
    },
))
ALL.append(factory(
    "factory_max_tokens_explicit",
    [msg("user_1", text="Short")],
    config={"modelId": "claude-3-5-sonnet", "maxTokens": 1000, "temperature": 0.3},
))
ALL.append(factory(
    "factory_ceiling_default_older_model",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-opus", "temperature": 0.5, "topP": 1.0},
))
ALL.append(factory(
    "factory_relay_prefix_ceiling",
    [msg("user_1", text="Hi")],
    config={"modelId": "anthropic/claude-3-opus"},
))
ALL.append(factory(
    "factory_top_p_thinking_below_range",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-opus-4-6", "topP": 0.9},
))

# ---- factory: cache control --------------------------------------------------------------------
ALL.append(factory(
    "factory_cache_disabled",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet", "anthropicCacheEnabled": False},
))
ALL.append(factory(
    "factory_cache_ttl_5m",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet", "anthropicCacheTtl": "5m"},
))
ALL.append(factory(
    "factory_cache_ttl_invalid",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet", "anthropicCacheTtl": "2h"},
))

# ---- factory: signed thinking replay and tool-round adaptation ----------------------------------
SIGNED_SEGS = [
    seg("thought", content="Let me check.", signature="sig-abc", signature_provider="Anthropic"),
    seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
        tool_call_id="call_1"),
]
RESULT_SEGS = [
    seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
        tool_result="Sunny", tool_call_id="call_1"),
]
ALL.append(factory(
    "factory_signed_round_replayed",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Anthropic:claude-opus-4-6",
         segments=SIGNED_SEGS),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_signed_round_foreign_provider_archived",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="OpenRouter:claude-opus-4-6",
         segments=[
             seg("thought", content="Let me check.", signature="sig-abc",
                 signature_provider="OpenRouter"),
             seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
                 tool_call_id="call_1"),
         ]),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_unsigned_round_archived",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Anthropic:claude-opus-4-6",
         segments=[
             seg("thought", content="Let me check."),
             seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
                 tool_call_id="call_1"),
         ]),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_round_without_calls_archived",
    [msg("user_1", text="Hi"),
     msg("tool_1", participant="MODEL", segments=[seg("answer", content="not a tool")]),
     msg("result_1", text="orphan output"),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_legacy_round_kept",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         tool_call=tc("get_weather", '{"location": "Oslo"}', "")),
     msg("result_1", tool_call=tc("get_weather", '{"location": "Oslo"}', "Sunny")),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-3-5-haiku"},
))
ALL.append(factory(
    "factory_legacy_round_archived_with_thinking",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         tool_call=tc("get_weather", '{"location": "Oslo"}', "")),
     msg("result_1", tool_call=tc("get_weather", '{"location": "Oslo"}', "Sunny")),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_orphan_result_dropped",
    [msg("user_1", text="Hello"),
     msg("result_1", text="stray result"),
     msg("user_2", text="More")],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_empty_tool_row_continue",
    [msg("user_1", text="Hi"),
     msg("tool_1", participant="MODEL"),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-3-5-haiku"},
))

# ---- factory: normal-message edges --------------------------------------------------------------
ALL.append(factory(
    "factory_whitespace_text_continue",
    [msg("user_1", text="   ")],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_assistant_images_dropped",
    [msg("user_1", text="draw"), msg("model_1", participant="MODEL", text="done",
                                     images=["files/a.png"])],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_images_disabled",
    [msg("user_1", text="look", images=["files/a.png"])],
    config={"modelId": "claude-3-5-sonnet", "includeImages": False},
))
ALL.append(factory(
    "factory_missing_image_skipped",
    [msg("user_1", text="look", images=["files/missing.png"])],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_consecutive_rows_coalesced",
    [msg("user_1", text="One"), msg("user_2", text="Two"),
     msg("model_1", participant="MODEL", text="Answer"),
     msg("model_2", participant="MODEL", text="More")],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_system_blank_rejected",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet"},
    system="   ",
))
ALL.append(factory(
    "factory_empty_messages_rejected",
    [],
    config={"modelId": "claude-3-5-sonnet"},
))
ALL.append(factory(
    "factory_nan_temperature_rejected",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet", "temperature": "NaN"},
))

# ---- factory: tool schemas ---------------------------------------------------------------------
NESTED_TOOL = {
    "type": "function",
    "function": {
        "name": "matrix",
        "description": "Nested schema",
        "parameters": {
            "type": "object",
            "properties": {
                "tags": {"type": "array", "description": "tags",
                         "items": {"type": "string", "description": "one tag"}},
                "point": {
                    "type": "object",
                    "description": "a point",
                    "properties": {"x": {"type": "number", "description": "x coordinate"}},
                    "required": ["x"],
                },
            },
            "required": ["tags"],
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
ALL.append(factory(
    "factory_tool_nested_schema",
    [msg("user_1", text="Go")],
    config={"modelId": "claude-3-5-sonnet", "tools": [NESTED_TOOL]},
))
ALL.append(factory(
    "factory_tool_external_schema_passthrough",
    [msg("user_1", text="Go")],
    config={"modelId": "claude-3-5-sonnet", "tools": [EXTERNAL_TOOL]},
))

# ---- validate: passing shapes -------------------------------------------------------------------
ALL.append(validate("validate_ok_minimal", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_ok_tool_round", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "enabled", "budget_tokens": 2048},
    "messages": [
        {"role": "user", "content": USER_TEXT},
        {"role": "assistant", "content": [
            part("thinking", thinking="hmm", signature="sig"),
            part("tool_use", id="call_a", name="lookup", input={"q": 1}),
        ]},
        {"role": "user", "content": [
            part("tool_result", tool_use_id="call_a", content="done"),
            part("text", text="next"),
        ]},
    ],
}))

# ---- validate: structural violations -------------------------------------------------------------
ALL.append(validate("validate_repeats_role", {
    "model": "claude-3-5-sonnet",
    "messages": [
        {"role": "user", "content": USER_TEXT},
        {"role": "user", "content": USER_TEXT},
    ],
}))
ALL.append(validate("validate_invalid_role", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "system", "content": USER_TEXT}],
}))
ALL.append(validate("validate_tool_result_after_text", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [
        part("text", text="hi"),
        part("tool_result", tool_use_id="call_a", content="x"),
    ]}],
}))
ALL.append(validate("validate_thinking_after_text", {
    "model": "claude-opus-4-6",
    "messages": [
        {"role": "user", "content": USER_TEXT},
        {"role": "assistant", "content": [
            part("text", text="hi"),
            part("thinking", thinking="hmm", signature="sig"),
        ]},
    ],
}))
ALL.append(validate("validate_tool_use_missing_thinking", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "adaptive"},
    "messages": [
        {"role": "user", "content": USER_TEXT},
        {"role": "assistant", "content": [part("tool_use", id="call_a", name="lookup",
                                               input={})]},
    ],
}))
ALL.append(validate("validate_orphan_tool_result", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [
        part("tool_result", tool_use_id="call_x", content="x"),
    ]}],
}))
ALL.append(validate("validate_result_id_mismatch", {
    "model": "claude-3-5-sonnet",
    "messages": [
        {"role": "assistant", "content": [part("tool_use", id="call_a", name="lookup",
                                               input={})]},
        {"role": "user", "content": [part("tool_result", tool_use_id="call_b", content="x")]},
    ],
}))
ALL.append(validate("validate_result_id_duplicate", {
    "model": "claude-3-5-sonnet",
    "messages": [
        {"role": "assistant", "content": [part("tool_use", id="call_a", name="lookup",
                                               input={})]},
        {"role": "user", "content": [
            part("tool_result", tool_use_id="call_a", content="x"),
            part("tool_result", tool_use_id="call_a", content="y"),
        ]},
    ],
}))
ALL.append(validate("validate_duplicate_tool_use_id", {
    "model": "claude-3-5-sonnet",
    "messages": [
        {"role": "assistant", "content": [part("tool_use", id="call_a", name="lookup",
                                               input={})]},
        {"role": "user", "content": [part("tool_result", tool_use_id="call_a", content="x")]},
        {"role": "assistant", "content": [part("tool_use", id="call_a", name="lookup",
                                               input={})]},
    ],
}))
ALL.append(validate("validate_text_blank", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [part("text", text="   ")]}],
}))
ALL.append(validate("validate_image_on_assistant", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "assistant", "content": [
        part("image", source={"type": "base64", "media_type": "image/png", "data": "AAAA"}),
    ]}],
}))
ALL.append(validate("validate_thinking_unsigned", {
    "model": "claude-opus-4-6",
    "messages": [{"role": "assistant", "content": [part("thinking", thinking="hmm")]}],
}))
ALL.append(validate("validate_tool_use_bad_identity", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "assistant", "content": [
        part("tool_use", id="bad id!", name="bad name", input={}),
    ]}],
}))
ALL.append(validate("validate_tool_result_blank_id", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [
        part("tool_result", tool_use_id=" ", content="x"),
    ]}],
}))
ALL.append(validate("validate_unsupported_type", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [part("audio", text="x")]}],
}))
ALL.append(validate("validate_text_extra_field", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [part("text", text="hi", name="extra")]}],
}))
ALL.append(validate("validate_empty_content", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": []}],
}))
ALL.append(validate("validate_start_end_roles", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "assistant", "content": USER_TEXT}],
}))

# ---- validate: thinking configuration -------------------------------------------------------------
ALL.append(validate("validate_thinking_budget_low", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "enabled", "budget_tokens": 512},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_thinking_enabled_no_budget", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "enabled"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_adaptive_with_budget", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "adaptive", "budget_tokens": 2048},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_disabled_with_display", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "disabled", "display": "summarized"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_output_config_without_thinking", {
    "model": "claude-opus-4-6",
    "output_config": {"effort": "medium"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_output_config_with_enabled_thinking", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "enabled", "budget_tokens": 2048},
    "output_config": {"effort": "medium"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_thinking_type_invalid", {
    "model": "claude-opus-4-6",
    "thinking": {"type": "sometimes"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))

# ---- validate: scalar gates ----------------------------------------------------------------------
ALL.append(validate("validate_top_p_out_of_range", {
    "model": "claude-3-5-sonnet",
    "top_p": 1.5,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_max_tokens_zero", {
    "model": "claude-3-5-sonnet",
    "max_tokens": 0,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_model_blank", {
    "model": "  ",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))

# ---- validate: tools matrix ----------------------------------------------------------------------
ALL.append(validate("validate_tools_matrix", {
    "model": "claude-3-5-sonnet",
    "tools": [
        {"name": "bad name", "description": "x",
         "input_schema": {"type": "object"}},
        {"name": "lookup", "description": "x",
         "input_schema": {"type": "array"}},
        {"name": "lookup", "description": "x",
         "input_schema": {"type": "object"}},
    ],
    "messages": [{"role": "user", "content": USER_TEXT}],
}))

# ---- validate: kotlinx decode behaviour (class names are part of the traces) ---------------------
ALL.append(validate("validate_decode_model_null", {
    "model": None,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_quoted_int", {
    "model": "claude-3-5-sonnet",
    "max_tokens": "5000",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_unknown_keys", {
    "model": "claude-3-5-sonnet",
    "future_field": {"nested": 1},
    "messages": [{"role": "user", "content": [{"type": "text", "text": "hi",
                                               "future_part": True}]}],
}))
# Written by hand below: a Python dict would collapse the duplicate key.
# validate_decode_duplicate_max_tokens pins last-wins duplicate decoding.
ALL.append(validate("validate_decode_stream_string", {
    "model": "claude-3-5-sonnet",
    "stream": "true",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_max_tokens_float", {
    "model": "claude-3-5-sonnet",
    "max_tokens": 5.5,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_messages_missing", {
    "model": "claude-3-5-sonnet",
}))
ALL.append(validate("validate_decode_part_type_missing", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [{"text": "hi"}]}],
}))
ALL.append(validate("validate_decode_input_null", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "assistant", "content": [
        part("tool_use", id="call_a", name="lookup", input=None),
    ]}],
}))

# ---- serialized-body gate -------------------------------------------------------------------------
ALL.append(serialized("serialized_ok",
                            '{"model":"claude-3-5-sonnet","messages":[{"role":"user","content":[]}]}'))
ALL.append(serialized("serialized_missing_model", '{"messages":[{"role":"user"}]}'))
ALL.append(serialized("serialized_empty_messages",
                            '{"model":"claude-3-5-sonnet","messages":[]}'))
ALL.append(serialized("serialized_not_object", '[]'))

# ---- edge pins: case-insensitive issuer, models/ prefix, malformed args ----------------------------
ALL.append(factory(
    "factory_signed_round_case_insensitive_issuer",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="anthropic:CLAUDE-OPUS-4-6",
         segments=[
             seg("thought", content="Let me check.", signature="sig-abc",
                 signature_provider="anthropic"),
             seg("tool", tool_name="get_weather", tool_args='{"location": "Oslo"}',
                 tool_call_id="call_1"),
         ]),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-opus-4-6"},
))
ALL.append(factory(
    "factory_signed_round_models_prefix",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL", model_name="Anthropic:models/claude-opus-4-6",
         segments=SIGNED_SEGS),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "models/claude-opus-4-6"},
))
ALL.append(factory(
    "factory_tool_args_not_json",
    [msg("user_1", text="Weather?"),
     msg("tool_1", participant="MODEL",
         segments=[seg("tool", tool_name="get_weather", tool_args="not json",
                       tool_call_id="call_1")]),
     msg("result_1", segments=RESULT_SEGS),
     msg("user_2", text="Thanks!")],
    config={"modelId": "claude-3-5-haiku"},
))
ALL.append(factory(
    "factory_nan_top_p_rejected",
    [msg("user_1", text="Hi")],
    config={"modelId": "claude-3-5-sonnet", "topP": "NaN"},
))

# ---- edge pins: decode branches (int signs, range, wrong kinds) ------------------------------------
ALL.append(validate("validate_decode_negative_max_tokens", {
    "model": "claude-3-5-sonnet",
    "max_tokens": -5,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_int_min", {
    "model": "claude-3-5-sonnet",
    "max_tokens": -2147483648,
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_top_p_infinity", {
    "model": "claude-3-5-sonnet",
    "top_p": "Infinity",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_top_p_quoted_decimal", {
    "model": "claude-3-5-sonnet",
    "top_p": "0.5",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
# validate_decode_top_p_overflow is written by hand below: Python cannot represent 1e999.
ALL.append(validate("validate_decode_max_tokens_quoted_plus", {
    "model": "claude-3-5-sonnet",
    "max_tokens": "+5000",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_max_tokens_leading_zero", {
    "model": "claude-3-5-sonnet",
    "max_tokens": "007",
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_model_wrong_kind", {
    "model": {"name": "claude"},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_source_missing_media", {
    "model": "claude-3-5-sonnet",
    "messages": [{"role": "user", "content": [
        part("image", source={"type": "base64", "data": "AAAA"}),
    ]}],
}))
ALL.append(validate("validate_decode_output_config_missing_effort", {
    "model": "claude-3-5-sonnet",
    "output_config": {},
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_decode_tool_name_missing", {
    "model": "claude-3-5-sonnet",
    "tools": [{"description": "x", "input_schema": {"type": "object"}}],
    "messages": [{"role": "user", "content": USER_TEXT}],
}))
ALL.append(validate("validate_tools_name_too_long", {
    "model": "claude-3-5-sonnet",
    "tools": [{"name": "a" * 65, "description": "x", "input_schema": {"type": "object"}}],
    "messages": [{"role": "user", "content": USER_TEXT}],
}))


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for scenario in ALL:
        name = scenario["name"]
        path = os.path.join(SCENARIOS_DIR, name + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(scenario, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    # Hand-written: a Python dict would collapse the duplicate key before serialization.
    duplicate_path = os.path.join(SCENARIOS_DIR, "validate_decode_duplicate_max_tokens.json")
    with open(duplicate_path, "w", encoding="utf-8") as handle:
        handle.write(
            "{\n"
            '  "name": "validate_decode_duplicate_max_tokens",\n'
            '  "providerName": "Anthropic",\n'
            '  "mode": "validate",\n'
            '  "request": {\n'
            '    "model": "claude-3-5-sonnet",\n'
            '    "max_tokens": 1,\n'
            '    "max_tokens": 0,\n'
            '    "messages": [{"role": "user", "content": [{"type": "text", "text": "Hello"}]}]\n'
            "  }\n"
            "}\n"
        )
    # Hand-written: Python floats cannot represent 1e999 (it would serialize as Infinity).
    overflow_path = os.path.join(SCENARIOS_DIR, "validate_decode_top_p_overflow.json")
    with open(overflow_path, "w", encoding="utf-8") as handle:
        handle.write(
            "{\n"
            '  "name": "validate_decode_top_p_overflow",\n'
            '  "providerName": "Anthropic",\n'
            '  "mode": "validate",\n'
            '  "request": {\n'
            '    "model": "claude-3-5-sonnet",\n'
            '    "top_p": 1e999,\n'
            '    "messages": [{"role": "user", "content": [{"type": "text", "text": "Hello"}]}]\n'
            "  }\n"
            "}\n"
        )
    print(f"Wrote {len(ALL) + 2} scenarios")


if __name__ == "__main__":
    main()
