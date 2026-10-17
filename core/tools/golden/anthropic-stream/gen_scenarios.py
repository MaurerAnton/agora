#!/usr/bin/env python3
"""Generates the anthropic-stream transcripts under scenarios/.

Every scenario is a transcript for AnthropicStreamTraceDump: ops `L` (raw line through the
provider's read loop), `C` (direct captureParseError), `I` (direct reportIncompleteBlocks), `E`
(stop reading). One scenario (or a close pair) per branch of AnthropicStreamEventRouter plus the
model-decode shapes of the provider's Json configuration.

Run from this directory: python3 gen_scenarios.py
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

CITATION_CHAR = (
    '{"type":"char_location","cited_text":"x","document_index":0,'
    '"document_title":"Doc","start_char_index":0,"end_char_index":1}'
)
CITATION_CHAR_REORDERED = (
    '{"document_index":0,"cited_text":"x","document_title":"Doc",'
    '"start_char_index":0,"end_char_index":1,"type":"char_location"}'
)
CITATION_CHAR_ZERO_FLOAT = (
    '{"type":"char_location","cited_text":"x","document_index":0.0,'
    '"document_title":"Doc","start_char_index":0,"end_char_index":1}'
)
CITATION_WEB = (
    '{"type":"web_search_result_location","title":"Anthropic","url":"https://example.com/a",'
    '"encrypted_index":"enc1","cited_text":"quote"}'
)
CITATION_UNKNOWN = '{"type":"not_a_location","title":"T"}'
CITATION_NULL_TYPE = '{"type":null,"title":"T"}'


def j(value):
    return json.dumps(value, separators=(",", ":"), ensure_ascii=False)


def data(payload):
    return "L data: " + payload


def event(kind, **fields):
    return j({"type": kind, **fields})


def delta_event(index, delta, **fields):
    return event("content_block_delta", index=index, delta=delta, **fields)


def text_start(index, text=None, citations=None, **fields):
    block = {"type": "text"}
    if text is not None:
        block["text"] = text
    if citations is not None:
        block["citations"] = citations
    return event("content_block_start", index=index, content_block=block, **fields)


def thinking_start(index, thinking=None, signature=None):
    block = {"type": "thinking"}
    if thinking is not None:
        block["thinking"] = thinking
    if signature is not None:
        block["signature"] = signature
    return event("content_block_start", index=index, content_block=block)


def tool_start(index, id=None, name=None, input=None):
    block = {"type": "tool_use"}
    if id is not None:
        block["id"] = id
    if name is not None:
        block["name"] = name
    if input is not None:
        block["input"] = input
    return event("content_block_start", index=index, content_block=block)


def tool_delta(index, partial):
    return delta_event(index, {"type": "input_json_delta", "partial_json": partial})


def stop_block(index):
    return event("content_block_stop", index=index)


def message_delta(usage=None, stop_reason=None, **fields):
    payload = {"type": "message_delta"}
    if usage is not None:
        payload["usage"] = usage
    if stop_reason is not None:
        payload["delta"] = {"stop_reason": stop_reason}
    payload.update(fields)
    return j(payload)


# The probe surfaces the router's stored usage state without a real terminal event.
PROBE = message_delta(usage={})

SCENARIOS = {}
ORDER = []


def scenario(name, ops):
    assert name not in SCENARIOS, name
    SCENARIOS[name] = ops
    ORDER.append(name)


# ---------------------------------------------------------------------------------------------
# A. message_start / stored usage state.

scenario("aa_message_start_usage_all", [
    data(event("message_start", message={"usage": {
        "input_tokens": 10, "output_tokens": 3,
        "cache_creation_input_tokens": 2, "cache_read_input_tokens": 4}})),
    data(PROBE),
])
scenario("ab_message_start_usage_partial", [
    data(event("message_start", message={"usage": {"input_tokens": 5}})),
    data(PROBE),
])
scenario("ac_message_start_usage_negative", [
    data(event("message_start", message={"usage": {
        "input_tokens": -3, "output_tokens": -1,
        "cache_creation_input_tokens": -7, "cache_read_input_tokens": -9}})),
    data(PROBE),
])
scenario("ad_message_start_usage_null", [
    data(event("message_start", message={"usage": None})),
    data(PROBE),
])
scenario("ae_message_start_no_message", [
    data(event("message_start")),
    data(PROBE),
])
scenario("af_message_start_repeated_last_wins", [
    data(event("message_start", message={"usage": {"input_tokens": 4, "cache_read_input_tokens": 1}})),
    data(event("message_start", message={"usage": {"input_tokens": None, "cache_read_input_tokens": 9}})),
    data(PROBE),
])
scenario("ag_message_start_zero_tokens", [
    data(event("message_start", message={"usage": {"input_tokens": 0, "cache_read_input_tokens": 0}})),
    data(PROBE),
])

# ---------------------------------------------------------------------------------------------
# B. early returns inside route() skip the trailing streamError capture.

scenario("ba_early_return_start_without_block_error", [
    data(event("content_block_start", error={"type": "boom", "message": "m"})),
])
scenario("bb_early_return_delta_without_delta_error", [
    data(event("content_block_delta", index=0, error={"type": "boom", "message": "m"})),
])
scenario("bc_early_return_input_json_without_tool_error", [
    data(delta_event(0, {"type": "input_json_delta", "partial_json": "x"},
                     error={"type": "boom", "message": "m"})),
])
scenario("bd_early_return_blank_signature_error", [
    data(delta_event(0, {"type": "signature_delta", "signature": "  "},
                     error={"type": "boom", "message": "m"})),
])
scenario("be_capture_runs_after_message_stop_error", [
    data(event("message_stop", error={"type": "boom", "message": "m"})),
])
scenario("bf_capture_runs_after_untyped_delta_error", [
    data(delta_event(0, {"text": None}, error={"type": "boom", "message": "m"})),
])
scenario("bg_capture_runs_after_start_with_block_error", [
    data(text_start(0, text="hi", error={"type": "boom", "message": "m"})),
])
scenario("bh_capture_runs_after_stop_error", [
    data(tool_start(0, id="call_1", name="calc")),
    data(event("content_block_stop", index=0, error={"type": "boom", "message": "m"})),
])

# ---------------------------------------------------------------------------------------------
# C. text blocks.

scenario("ca_text_block_flow", [
    data(text_start(0, text="Hello ")),
    data(delta_event(0, {"type": "text_delta", "text": "world"})),
    data(stop_block(0)),
])
scenario("cb_text_start_empty_text", [
    data(text_start(0, text="")),
    data(delta_event(0, {"type": "text_delta", "text": "a"})),
    data(stop_block(0)),
])
scenario("cc_text_start_no_text", [
    data(text_start(0)),
    data(delta_event(0, {"type": "text_delta", "text": "a"})),
])
scenario("cd_text_delta_empty", [
    data(text_start(0, text="a")),
    data(delta_event(0, {"type": "text_delta", "text": ""})),
    data(delta_event(0, {"type": "text_delta", "text": None})),
    data(delta_event(0, {"type": "text_delta"})),
])
scenario("ce_text_delta_wrong_block", [
    data(thinking_start(0, thinking="t")),
    data(delta_event(0, {"type": "text_delta", "text": "lost"})),
])
scenario("cf_text_citation_at_start", [
    data(text_start(0, text="ab")),
    data(stop_block(0)),
])
scenario("cg_text_citation_delta_reemits", [
    data(text_start(0, text="a")),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
    data(delta_event(0, {"type": "text_delta", "text": "b"})),
    data(stop_block(0)),
])
scenario("ch_text_citation_dedup_identical", [
    data(text_start(0, text="a")),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
    data(stop_block(0)),
])
scenario("ci_text_citation_dedup_key_order", [
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR_REORDERED)})),
    data(stop_block(0)),
])
scenario("cj_text_citation_not_dedup_number_form", [
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR_ZERO_FLOAT)})),
    data(stop_block(0)),
])
scenario("ck_text_citation_unknown_type", [
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_UNKNOWN)})),
])
scenario("cl_text_citation_null_type", [
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_NULL_TYPE)})),
])
scenario("cm_text_citation_non_object", [
    data(delta_event(0, {"type": "citations_delta", "citation": "just a string"})),
])
scenario("cn_text_citation_delta_without_block", [
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
])
scenario("co_text_citation_on_tool_block", [
    data(tool_start(0, id="call_1", name="calc")),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_CHAR)})),
])
scenario("cp_text_citations_cap", [
    data(text_start(0, text="a")),
] + [
    data(delta_event(0, {"type": "citations_delta", "citation": {
        "type": "char_location", "document_index": i, "document_title": f"Doc{i}",
        "start_char_index": 0, "end_char_index": 1, "cited_text": "x"}}))
    for i in range(101)
] + [
    data(stop_block(0)),
])
scenario("cq_text_start_citations_array", [
    data(text_start(0, text="ab", citations=[json.loads(CITATION_WEB), json.loads(CITATION_CHAR)])),
    data(stop_block(0)),
])

# ---------------------------------------------------------------------------------------------
# D. thinking blocks.

scenario("da_thinking_flow", [
    data(thinking_start(0, thinking="Let me ", signature="sig1")),
    data(delta_event(0, {"type": "thinking_delta", "thinking": "think"})),
    data(delta_event(0, {"type": "signature_delta", "signature": "sig2"})),
    data(delta_event(0, {"type": "thinking_delta", "thinking": "more"})),
    data(stop_block(0)),
])
scenario("db_thinking_blank_signature", [
    data(thinking_start(0, thinking="t", signature="  ")),
])
scenario("dc_thinking_empty_thought", [
    data(thinking_start(0, thinking="", signature="sig")),
])
scenario("dd_thinking_no_signature", [
    data(thinking_start(0, thinking="t")),
    data(delta_event(0, {"type": "thinking_delta", "thinking": "u"})),
])
scenario("de_signature_delta_blank", [
    data(delta_event(0, {"type": "signature_delta", "signature": ""})),
    data(delta_event(0, {"type": "signature_delta", "signature": None})),
])
scenario("df_thinking_delta_empty", [
    data(delta_event(0, {"type": "thinking_delta", "thinking": ""})),
    data(delta_event(0, {"type": "thinking_delta", "thinking": None})),
])
scenario("dg_thinking_delta_after_stop", [
    data(thinking_start(0, thinking="t", signature="s")),
    data(stop_block(0)),
    data(delta_event(0, {"type": "thinking_delta", "thinking": "late"})),
])
scenario("dh_signature_delta_after_stop", [
    data(stop_block(0)),
    data(delta_event(0, {"type": "signature_delta", "signature": "late"})),
    data(delta_event(0, {"type": "signature_delta", "signature": "later"})),
])

# ---------------------------------------------------------------------------------------------
# E. tool blocks.

scenario("ea_tool_full_flow", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, '{"a":')),
    data(tool_delta(0, "1}")),
    data(stop_block(0)),
])
scenario("eb_tool_initial_input", [
    data(tool_start(0, id="toolu_1", name="calc", input={"a": 1})),
    data(stop_block(0)),
])
scenario("ec_tool_initial_empty_input", [
    data(tool_start(0, id="toolu_1", name="calc", input={})),
    data(stop_block(0)),
])
scenario("ed_tool_input_null", [
    data(tool_start(0, id="toolu_1", name="calc", input=None)),
    data(stop_block(0)),
])
scenario("ee_tool_blank_id", [
    data(tool_start(0, id="", name="calc")),
    data(tool_delta(0, "{}")),
    data(stop_block(0)),
])
scenario("ef_tool_missing_id", [
    data(tool_start(0, name="calc")),
    data(stop_block(0)),
])
scenario("eg_tool_blank_name", [
    data(tool_start(0, id="toolu_1", name="")),
    data(tool_delta(0, '{"a":1}')),
    data(stop_block(0)),
])
scenario("eh_tool_name_64", [
    data(tool_start(0, id="toolu_1", name="a" * 64)),
    data(stop_block(0)),
])
scenario("ei_tool_name_65", [
    data(tool_start(0, id="toolu_1", name="a" * 65)),
    data(stop_block(0)),
])
scenario("ej_tool_id_invalid_char", [
    data(tool_start(0, id="tool id!", name="calc")),
    data(stop_block(0)),
])
scenario("ek_tool_id_129", [
    data(tool_start(0, id="a" * 129, name="calc")),
    data(stop_block(0)),
])
scenario("el_tool_id_128", [
    data(tool_start(0, id="a" * 128, name="calc")),
    data(stop_block(0)),
])
scenario("em_tool_duplicate_ids", [
    data(tool_start(0, id="call_same", name="fa")),
    data(stop_block(0)),
    data(tool_start(1, id="call_same", name="fb")),
    data(stop_block(1)),
])
scenario("en_tool_failed_completion_does_not_reserve_id", [
    data(tool_start(0, id="call_same", name="fa")),
    data(tool_delta(0, "not json")),
    data(stop_block(0)),
    data(tool_start(1, id="call_same", name="fb")),
    data(stop_block(1)),
])
scenario("eo_tool_args_truncated", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, '{"a":')),
    data(stop_block(0)),
])
scenario("ep_tool_args_array", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, "[1,2]")),
    data(stop_block(0)),
])
scenario("eq_tool_args_primitive", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, "42")),
    data(stop_block(0)),
])
scenario("er_tool_args_invalid_json", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, "nope")),
    data(stop_block(0)),
])
scenario("es_tool_args_blank_completes", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(stop_block(0)),
])
scenario("et_tool_args_trailing_garbage", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, "{} x")),
    data(stop_block(0)),
])
scenario("eu_tool_args_duplicate_keys", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, '{"a":1,"a":2}')),
    data(stop_block(0)),
])
scenario("ev_tool_input_duplicate_keys", [
    data(tool_start(0, id="toolu_1", name="calc", input={"a": 1, "a": 2})),
    data(stop_block(0)),
])
scenario("ew_tool_snapshot_resend", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, '{"a":1')),
    data(tool_delta(0, '{"a":1')),
    data(tool_delta(0, '{"a":1,"b":2}')),
    data(tool_delta(0, '{"a":1,"b":2}')),
    data(stop_block(0)),
])
scenario("ex_tool_stale_short_resend", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, '{"long_enough":1}')),
    data(tool_delta(0, '{"long_')),
    data(stop_block(0)),
])
scenario("ey_tool_empty_and_null_deltas", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(tool_delta(0, "")),
    data(tool_delta(0, None)),
    data(delta_event(0, {"type": "input_json_delta"})),
    data(stop_block(0)),
])
scenario("ez_tool_delta_without_block", [
    data(tool_delta(0, "x")),
])
scenario("fa_tool_delta_after_stop", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(stop_block(0)),
    data(tool_delta(0, "x")),
])
scenario("fb_tool_untyped_delta", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(delta_event(0, {"partial_json": '"b":2}'})),
    data(delta_event(0, {})),
    data(delta_event(0, {"type": None, "partial_json": '"c":3}'})),
    data(stop_block(0)),
])
scenario("fc_tool_stop_twice", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data(stop_block(0)),
    data(stop_block(0)),
])
scenario("fd_tool_stop_unknown_index", [
    data(stop_block(7)),
])
scenario("fe_tool_id_underscore_dash", [
    data(tool_start(0, id="_call-1", name="do_it-now")),
    data(stop_block(0)),
])
scenario("ff_tool_name_space_suffix", [
    data(tool_start(0, id="toolu_1", name="calc ")),
    data(stop_block(0)),
])
scenario("fg_tool_initial_input_then_delta", [
    data(tool_start(0, id="toolu_1", name="calc", input={"a": 1})),
    data(tool_delta(0, "}")),
    data(stop_block(0)),
])

# ---------------------------------------------------------------------------------------------
# F. index tracking / synthetic indices.

scenario("ga_index_null_start", [
    data(text_start(None, text="a")),
    data(delta_event(None, {"type": "text_delta", "text": "b"})),
])
scenario("gb_index_null_start_then_explicit_then_null", [
    data(text_start(None, text="a")),
    data(text_start(5, text="b")),
    data(text_start(None, text="c")),
])
scenario("gc_index_null_continuation_before_any", [
    data(delta_event(None, {"type": "text_delta", "text": "a"})),
    data(delta_event(None, {"type": "text_delta", "text": "b"})),
    data(text_start(None, text="c")),
])
scenario("gd_index_null_continuation_after_start", [
    data(text_start(3, text="a")),
    data(delta_event(None, {"type": "text_delta", "text": "b"})),
])
scenario("ge_index_null_continuation_after_stop", [
    data(text_start(3, text="a")),
    data(stop_block(3)),
    data(delta_event(None, {"type": "text_delta", "text": "late"})),
])
scenario("gf_index_negative", [
    data(text_start(-1, text="a")),
    data(delta_event(None, {"type": "text_delta", "text": "b"})),
])
scenario("gg_index_null_delta_advances_synthetic", [
    data(event("content_block_delta")),
    data(text_start(None, text="a")),
])
scenario("gh_index_zero", [
    data(text_start(0, text="a")),
])
scenario("gi_index_int_max", [
    data(text_start(2147483647, text="a")),
])

# ---------------------------------------------------------------------------------------------
# G. message_delta usage math.

scenario("ha_usage_delta_full", [
    data(event("message_start", message={"usage": {
        "input_tokens": 10, "cache_creation_input_tokens": 2, "cache_read_input_tokens": 3}})),
    data(message_delta(usage={"input_tokens": 10, "output_tokens": 5,
                              "cache_creation_input_tokens": 2, "cache_read_input_tokens": 3})),
])
scenario("hb_usage_delta_output_only", [
    data(message_delta(usage={"output_tokens": 7})),
])
scenario("hc_usage_delta_negative", [
    data(message_delta(usage={"input_tokens": -5, "output_tokens": -1,
                              "cache_creation_input_tokens": -2, "cache_read_input_tokens": -3})),
])
scenario("hd_usage_delta_cache_read_only", [
    data(event("message_start", message={"usage": {"input_tokens": 4}})),
    data(message_delta(usage={"cache_read_input_tokens": 6})),
])
scenario("he_usage_delta_saturates", [
    data(message_delta(usage={"input_tokens": 2147483647, "output_tokens": 2147483647,
                              "cache_creation_input_tokens": 1, "cache_read_input_tokens": 1})),
])
scenario("hf_usage_delta_none", [
    data(message_delta(stop_reason="end_turn")),
])
scenario("hg_usage_delta_empty_usage", [
    data(event("message_start", message={"usage": {"input_tokens": 10}})),
    data(message_delta(usage={})),
])
scenario("hh_usage_adopts_start_then_delta_overrides", [
    data(event("message_start", message={"usage": {"input_tokens": 1, "cache_read_input_tokens": 1}})),
    data(message_delta(usage={"input_tokens": 20, "output_tokens": 2})),
])
scenario("hi_usage_partial_keep_stored", [
    data(event("message_start", message={"usage": {"input_tokens": 3, "cache_read_input_tokens": 5}})),
    data(message_delta(usage={"output_tokens": 2})),
])

# ---------------------------------------------------------------------------------------------
# H. terminal markers / stop reason.

scenario("ia_message_stop_plain", [
    data(event("message_stop")),
    data(text_start(0, text="ignored")),
])
scenario("ib_message_stop_with_delta_reason", [
    data(event("message_stop", delta={"stop_reason": "End_Turn"})),
])
scenario("ic_message_stop_with_message_reason", [
    data(event("message_stop", message={"stop_reason": "max_tokens"})),
])
scenario("id_stop_reason_blank", [
    data(message_delta(stop_reason="  ")),
    data(event("message_stop")),
])
scenario("ie_stop_reason_overwrite", [
    data(message_delta(stop_reason="End_Turn")),
    data(message_delta(stop_reason="tool_use")),
])
scenario("if_message_delta_delta_null_message_reason", [
    data(event("message_delta", message={"stop_reason": "stop_sequence"}, usage={})),
])
scenario("ig_message_delta_delta_reason_precedence", [
    data(event("message_delta", delta={"stop_reason": "tool_use"},
               message={"stop_reason": "end_turn"}, usage={})),
])
scenario("ih_stop_reason_unicode_case", [
    data(message_delta(stop_reason="MAX_TOKENS")),
])
scenario("ii_message_stop_after_stop_reason", [
    data(message_delta(stop_reason="end_turn")),
    data(event("message_stop")),
])

# ---------------------------------------------------------------------------------------------
# I. in-band errors.

scenario("ja_error_event", [
    data(event("error", error={"type": "overloaded_error", "message": "Overloaded"})),
    data(text_start(0, text="ignored")),
])
scenario("jb_error_event_no_message", [
    data(event("error", error={"type": "overloaded_error"})),
])
scenario("jc_error_event_blank_message", [
    data(event("error", error={"type": "x", "message": "   "})),
])
scenario("jd_error_event_null_error", [
    data(event("error")),
])
scenario("je_error_field_on_unknown_type", [
    data(event("weird_event", error={"type": "boom", "message": "m"})),
])
scenario("jf_error_field_on_content_block_start_with_block", [
    data(text_start(0, text="a", error={"type": "boom", "message": "m"})),
])
scenario("jg_error_field_on_parse_error_never_captured", [
    data('{"type":"content_block_start","error":{"type":"boom","message":"m"}} trailing'),
])
scenario("jh_failed_to_generate_outcome", [
    data(event("ping", outcome="Failed To Generate a response")),
    data(text_start(0, text="ignored")),
])
scenario("ji_failed_to_generate_on_message_start", [
    data(event("message_start", outcome="upstream failed to generate")),
])
scenario("jj_outcome_unrelated", [
    data(event("ping", outcome="all good")),
    data(text_start(0, text="a")),
])
scenario("jk_outcome_failed_to_generate_generic_error_field_first", [
    data(event("ping", outcome="failed to generate", error={"type": "t", "message": "m"})),
])
scenario("jl_failed_to_generate_empty_outcome", [
    data(event("ping", outcome="")),
])
scenario("jm_error_after_error_kept_first", [
    data(event("error", error={"type": "first", "message": "1"})),
    data(event("weird_event", error={"type": "second", "message": "2"})),
])

# ---------------------------------------------------------------------------------------------
# J. decode failures of the provider's Json configuration.

scenario("ka_parse_error_garbage", [
    data("zzz"),
])
scenario("kb_parse_error_trailing", [
    data('{"type":"ping"} x'),
])
scenario("kc_parse_error_root_array", [
    data("[1]"),
])
scenario("kd_parse_error_truncated", [
    data('{"type":"mess'),
])
scenario("ke_parse_error_type_missing", [
    data('{"delta":{}}'),
])
scenario("kf_parse_error_type_number", [
    data('{"type":5}'),
])
scenario("kg_parse_error_type_null", [
    data('{"type":null}'),
])
scenario("kh_parse_error_delta_string", [
    data('{"type":"content_block_delta","delta":"x"}'),
])
scenario("ki_parse_error_content_block_string", [
    data('{"type":"content_block_start","content_block":"x"}'),
])
scenario("kj_parse_error_usage_array", [
    data('{"type":"message_start","message":{"usage":[]}}'),
])
scenario("kk_parse_error_outcome_number", [
    data('{"type":"ping","outcome":123}'),
])
scenario("kl_parse_error_error_message_number", [
    data('{"type":"error","error":{"message":5}}'),
])
scenario("km_parse_error_index_float", [
    data('{"type":"content_block_start","index":1.5,"content_block":{"type":"text"}}'),
])
scenario("kn_parse_error_index_overflow", [
    data('{"type":"content_block_start","index":2147483648,"content_block":{"type":"text"}}'),
])
scenario("ko_parse_error_citations_not_array", [
    data('{"type":"content_block_start","index":0,"content_block":{"type":"text","citations":{}}}'),
])
scenario("kp_parse_error_input_not_object", [
    data('{"type":"content_block_start","index":0,"content_block":{"type":"tool_use","input":[1]}}'),
])
scenario("kq_parse_error_bad_escape", [
    data('{"type":"ping","outcome":"a\\qb"}'),
])
scenario("kr_parse_error_bom", [
    data("\ufeff" + '{"type":"ping"}'),
])
scenario("ks_parse_error_empty_payload", [
    "L data: ",
])
scenario("kt_parse_error_long_payload_take512", [
    data('{"type":"ping"}' + "z" * 620),
])
scenario("ku_parse_error_take512_utf16_split", [
    data("a" * 11 + "\U0001f600" * 300),
])
scenario("kv_parse_error_take512_utf16_even", [
    data("a" * 10 + "\U0001F600" * 300),
])

# ---------------------------------------------------------------------------------------------
# K. accepted decode shapes.

scenario("la_decode_unknown_keys", [
    data('{"type":"ping","foo":1,"bar":[1,2],"baz":{"x":null}}'),
    data(text_start(0, text="a")),
])
scenario("lb_decode_duplicate_type_last_wins", [
    data('{"type":"content_block_start","type":"ping"}'),
    data(text_start(0, text="a")),
])
scenario("lc_decode_usage_quoted_ints", [
    data(event("message_start", message={"usage": {}}).replace('"usage":{}',
         '"usage":{"input_tokens":"5","cache_read_input_tokens":"7"}')),
    data(PROBE),
])
scenario("ld_decode_usage_explicit_nulls", [
    data('{"type":"message_start","message":{"usage":{"input_tokens":null,"output_tokens":null}}}'),
    data(PROBE),
])
scenario("le_decode_error_object_missing_fields", [
    data('{"type":"error","error":{}}'),
])
scenario("lf_decode_delta_null", [
    data('{"type":"content_block_delta","index":0,"delta":null}'),
])
scenario("lg_decode_index_quoted", [
    data('{"type":"content_block_start","index":"1","content_block":{"type":"text","text":"a"}}'),
    data('{"type":"content_block_delta","index":"1","delta":{"type":"text_delta","text":"b"}}'),
])
scenario("lh_decode_index_exponent", [
    data('{"type":"content_block_start","index":1e2,"content_block":{"type":"text","text":"a"}}'),
])
scenario("li_decode_index_negative_quoted", [
    data('{"type":"content_block_start","index":"-2","content_block":{"type":"text","text":"a"}}'),
])
scenario("lj_decode_citations_array_of_any", [
    data('{"type":"content_block_start","index":0,"content_block":{"type":"text","text":"a",'
         '"citations":[{"type":"char_location","document_title":"D","document_index":0},'
         '"plain",5,null]}}'),
])
scenario("lm_decode_content_block_input_null", [
    data('{"type":"content_block_start","index":0,"content_block":{"type":"tool_use","id":"c1",'
         '"name":"n","input":null}}'),
    data(stop_block(0)),
])
scenario("ln_decode_unicode_text_and_escapes", [
    data(text_start(0, text="a\u2028b\tc\rd\"e|f;g\\h\u00e9\U0001f600")),
    data(delta_event(0, {"type":"text_delta", "text": "\u2029tail"})),
])
scenario("lo_decode_raw_tab_in_string", [
    data('{"type":"error","error":{"type":"t","message":"a\tb"}}'),
])
scenario("lp_decode_delta_type_empty", [
    data('{"type":"content_block_delta","index":0,"delta":{"type":"","partial_json":"x"}}'),
])
scenario("lq_decode_dup_index_last_wins", [
    data('{"type":"content_block_start","index":0,"index":1,"content_block":{"type":"text","text":"a"}}'),
])
scenario("lr_decode_dup_content_block", [
    data('{"type":"content_block_start","index":0,"content_block":{"type":"text","text":"a"},'
         '"content_block":{"type":"text","text":"b"}}'),
])
scenario("ls_decode_message_usage_and_stop_reason", [
    data(event("message_start", message={"usage": {"input_tokens": 2}, "stop_reason": "end_turn"})),
    data(PROBE),
])
scenario("lt_decode_stop_reason_number_rejected", [
    data('{"type":"message_stop","message":{"stop_reason":7}}'),
])

# ---------------------------------------------------------------------------------------------
# L. captureParseError op and reportIncompleteBlocks op.

scenario("ma_capture_direct_then_second_ignored", [
    "C first raw line",
    "C second raw line",
])
scenario("mb_capture_after_existing_stream_error", [
    data(event("error", error={"type": "t", "message": "m"})),
    "C never stored",
])
scenario("mc_capture_take512", [
    "C " + "q" * 600,
])
scenario("md_capture_empty", [
    "C ",
])
scenario("me_capture_after_loop_stop", [
    data(event("message_stop")),
    "C after stop",
])
scenario("mf_capture_utf16_split", [
    "C " + "b" * 11 + "\U0001f600" * 300,
])
scenario("mg_incomplete_none", [
    data(event("message_stop")),
])
scenario("mh_incomplete_valid_name_open", [
    data(tool_start(0, id="call_1", name="calc")),
])
scenario("mi_incomplete_bad_name_open", [
    data(tool_start(0, id="call_1", name="bad name")),
])
scenario("mj_incomplete_sorted_by_index_bad_first", [
    data(tool_start(1, id="call_1", name="calc")),
    data(tool_start(0, id="call_2", name="bad name")),
])
scenario("mk_incomplete_sorted_by_index_good_first", [
    data(tool_start(0, id="call_1", name="calc")),
    data(tool_start(1, id="call_2", name="bad name")),
])
scenario("ml_incomplete_twice_emits_twice", [
    data(tool_start(0, id="call_1", name="bad name")),
    "I",
    "I",
])
scenario("mm_incomplete_after_closed_block", [
    data(tool_start(0, id="call_1", name="calc")),
    data(stop_block(0)),
    "I",
])
scenario("mn_incomplete_after_malformed_stop", [
    data(tool_start(0, id="call_1", name="bad name")),
    data(tool_delta(0, "{}")),
    data(stop_block(0)),
    "I",
])
scenario("mo_incomplete_name_64_open", [
    data(tool_start(0, id="call_1", name="a" * 64)),
])
scenario("mp_incomplete_name_65_open", [
    data(tool_start(0, id="call_1", name="a" * 65)),
])
scenario("mq_incomplete_empty_name_open", [
    data(tool_start(0, id="call_1")),
])
scenario("mr_incomplete_report_sets_reported_error", [
    data(tool_start(0, id="call_1", name="")),
    data(text_start(1, text="a")),
])

# ---------------------------------------------------------------------------------------------
# M. line-shape and loop gating.

scenario("na_line_not_data", [
    "L hello world",
    data(text_start(0, text="a")),
])
scenario("nb_line_data_no_space", [
    "L data:{\"type\":\"message_stop\"}",
    data(text_start(0, text="a")),
])
scenario("nc_line_data_trim", [
    "L data:   " + event("message_stop") + "   ",
])
scenario("nd_blank_line", [
    "L ",
    data(text_start(0, text="a")),
])
scenario("ne_lines_after_stop_ignored", [
    data(event("message_stop")),
    data(text_start(0, text="x")),
    "L data: zzz",
])
scenario("nf_lines_after_error_ignored", [
    data(event("error", error={"type": "t", "message": "m"})),
    data(text_start(0, text="x")),
])
scenario("ng_end_op_stops_reading", [
    data(text_start(0, text="a")),
    "E",
    data(text_start(1, text="ignored")),
])
scenario("nh_stream_error_stops_after_delta", [
    data(event("message_start", message={"usage": {"input_tokens": 1}})),
    data(event("weird_event", error={"type": "t", "message": "m"})),
    data(PROBE),
])

# ---------------------------------------------------------------------------------------------
# N. combined flows.

scenario("oa_full_anthropic_stream", [
    data(event("message_start", message={"usage": {
        "input_tokens": 100, "cache_creation_input_tokens": 10, "cache_read_input_tokens": 20}})),
    data(text_start(0, text="According to ")),
    data(delta_event(0, {"type": "citations_delta", "citation": json.loads(CITATION_WEB)})),
    data(delta_event(0, {"type": "text_delta", "text": "the docs"})),
    data(stop_block(0)),
    data(thinking_start(1, thinking="Reasoning", signature="sig")),
    data(delta_event(1, {"type": "thinking_delta", "thinking": " more"})),
    data(delta_event(1, {"type": "signature_delta", "signature": "final"})),
    data(stop_block(1)),
    data(tool_start(2, id="toolu_9", name="search", input={"q": "docs"})),
    data(tool_delta(2, ',"limit":3}')),
    data(stop_block(2)),
    data(message_delta(usage={"output_tokens": 42, "input_tokens": 100,
                              "cache_creation_input_tokens": 10,
                              "cache_read_input_tokens": 20}, stop_reason="tool_use")),
    data(event("message_stop")),
    data(event("ping")),
])
scenario("ob_truncated_mid_tool", [
    data(text_start(0, text="partial")),
    data(stop_block(0)),
    data(tool_start(1, id="toolu_1", name="calc")),
    data(tool_delta(1, '{"a":')),
])
scenario("oc_error_mid_stream_with_open_text", [
    data(text_start(0, text="a")),
    data(event("error", error={"type": "overloaded_error", "message": "Overloaded"})),
])
scenario("od_parse_error_mid_stream_with_open_tool", [
    data(tool_start(0, id="toolu_1", name="calc")),
    data("not json"),
])
scenario("oe_usage_rounded_by_caches", [
    data(event("message_start", message={"usage": {
        "input_tokens": 7, "cache_creation_input_tokens": 5, "cache_read_input_tokens": 11}})),
    data(message_delta(usage={"output_tokens": 3})),
    data(event("message_stop")),
])
scenario("of_text_multiple_blocks_same_answer", [
    data(text_start(0, text="one")),
    data(stop_block(0)),
    data(text_start(1, text="two")),
    data(stop_block(1)),
])


def main():
    os.makedirs(OUT, exist_ok=True)
    for stale in os.listdir(OUT):
        if stale.endswith(".txt"):
            os.remove(os.path.join(OUT, stale))
    width = max(3, len(str(len(ORDER))))
    for index, name in enumerate(ORDER, start=1):
        path = os.path.join(OUT, f"{index:0{width}d}_{name}.txt")
        with open(path, "w", encoding="utf-8") as handle:
            handle.write("\n".join(SCENARIOS[name]) + "\n")
    print(f"Wrote {len(ORDER)} scenarios into {OUT}")


if __name__ == "__main__":
    main()
