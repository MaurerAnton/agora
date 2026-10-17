#!/usr/bin/env python3
"""Generates the scenario corpus for the Gemini stream golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def scenario(name, ops, provider="Google"):
    ALL.append({"name": name, "providerName": provider, "ops": ops})


def data(payload):
    return "L data: " + json.dumps(payload, separators=(",", ":"), ensure_ascii=False)


def raw(line):
    return "L " + line


END = "E"


def candidate(*parts, finish=None, grounding=None):
    content = {"role": "model", "parts": list(parts)}
    out = {"content": content}
    if finish is not None:
        out["finishReason"] = finish
    if grounding is not None:
        out["groundingMetadata"] = grounding
    return {"candidates": [out]}


def usage(**kw):
    return {"usageMetadata": kw}


# ---- basic text --------------------------------------------------------------------------------
scenario("stream_basic_text", [
    data(candidate({"text": "Hel"}, {"text": "lo"})),
    data(candidate({"text": " world"}, finish="STOP")),
    END,
])
scenario("stream_non_data_lines_ignored", [
    raw("event: message"),
    raw(": keep-alive comment"),
    raw(""),
    data(candidate({"text": "hi"}, finish="STOP")),
    END,
])
scenario("stream_usage_only_line", [
    data(usage(promptTokenCount=7)),
    data(candidate({"text": "hi"}, finish="STOP")),
    END,
])
scenario("stream_empty_data_is_parse_error", [
    raw("data: "),
    data(candidate({"text": "late"})),
    END,
])

# ---- thoughts ---------------------------------------------------------------------
scenario("stream_thought_string_bold_title", [
    data(candidate({"thought": "**Summary** of the reasoning steps."})),
    data(candidate({"text": "done"}, finish="STOP")),
    END,
])
scenario("stream_thought_heading_title", [
    data(candidate({"thought": "## Headline\nBody of the thought."})),
    data(candidate({"text": "done"}, finish="STOP")),
    END,
])
scenario("stream_thought_empty_and_non_string_markers", [
    data(candidate({"thought": ""}, {"thought": 5}, {"text": "plain answer"})),
    data(candidate({"thought": True}, {"text": "in thought"}, {"text": "after thought"})),
    data(candidate({"text": "final"}, finish="STOP")),
    END,
])
scenario("stream_thought_signature_routes_text", [
    data(candidate(
        {"thoughtSignature": "sig-1"},
        {"text": "thinking aloud"},
        {"text": "still thinking"},
    )),
    data(candidate({"text": "answered"}, finish="STOP")),
    END,
])
scenario("stream_reasoning_content", [
    data(candidate({"reasoning_content": "**Derived** the answer."})),
    data(candidate({"text": "answer"}, finish="STOP")),
    END,
])

# ---- function calls -----------------------------------------------------------------------------
scenario("stream_function_call_valid", [
    data(candidate({"functionCall": {"id": "call_1", "name": "lookup",
                                     "args": {"q": "agora"}}})),
    data(candidate({"text": "checking"}, finish="STOP")),
    END,
])
scenario("stream_function_call_missing_or_blank_id", [
    data(candidate({"functionCall": {"name": "lookup", "args": {}}})),
    data(candidate({"functionCall": {"id": "", "name": "other"}})),
    data(candidate(finish="STOP")),
    END,
])
scenario("stream_function_call_no_args", [
    data(candidate({"functionCall": {"id": "call_1", "name": "lookup"}})),
    data(candidate({"functionCall": {"id": "call_2", "name": "bare", "args": {}}})),
    data(candidate(finish="STOP")),
    END,
])
scenario("stream_function_call_duplicate_id", [
    data(candidate({"functionCall": {"id": "call_1", "name": "lookup", "args": {"a": 1}}})),
    data(candidate({"functionCall": {"id": "call_1", "name": "lookup", "args": {"a": 2}}})),
    data(candidate(finish="STOP")),
    END,
])
scenario("stream_function_call_unsafe_name", [
    data(candidate({"functionCall": {"id": "call_1", "name": "bad name", "args": {}}})),
    data(candidate(finish="STOP")),
    END,
])
scenario("stream_function_call_signature_chain", [
    data(candidate(
        {"thoughtSignature": "s1", "functionCall": {"id": "c1", "name": "one", "args": {}}},
    )),
    data(candidate(
        {"functionCall": {"id": "c2", "name": "two", "args": {}, "thought_signature": "s2"}},
    )),
    data(candidate(
        {"thoughtSignature": "s3"},
        {"functionCall": {"id": "c3", "name": "three"}},
    )),
    data(candidate(
        {"functionCall": {"id": "c4", "name": "four"}},
    )),
    data(candidate(finish="STOP")),
    END,
])

# ---- code execution -------------------------------------------------------------------------------
scenario("stream_code_execution_round", [
    data(candidate({"executableCode": {"language": "python", "code": "print(1)"}})),
    data(candidate({"codeExecutionResult": {"outcome": "OUTCOME_OK", "output": "1"}})),
    data(candidate({"text": "done"}, finish="STOP")),
    END,
])
scenario("stream_code_execution_queue_pairing", [
    data(candidate(
        {"executableCode": {"language": "python", "code": "a"}},
        {"executableCode": {"language": "python", "code": "b"}},
    )),
    data(candidate({"codeExecutionResult": {"outcome": "OUTCOME_OK", "output": "1"}})),
    data(candidate({"codeExecutionResult": {"outcome": "FAILED", "output": "boom"}})),
    data(candidate(finish="STOP")),
    END,
])
scenario("stream_code_execution_result_without_pending", [
    data(candidate({"codeExecutionResult": {"outcome": "OK", "output": "0"}})),
    data(candidate({"codeExecutionResult": {"outcome": "failed", "output": "x"}})),
    data(candidate(finish="STOP")),
    END,
])

# ---- finish reasons and stopping ----------------------------------------------------------------------
scenario("stream_finish_reason_normalized", [
    data(candidate({"text": "a"}, finish="MAX_TOKENS")),
    END,
])
scenario("stream_finish_reason_hyphen_form", [
    data(candidate({"text": "a"}, finish="max-tokens")),
    END,
])
scenario("stream_finish_reason_lowercase_passthrough", [
    data(candidate({"text": "a"}, finish="STOP")),
    END,
])
scenario("stream_break_on_finish_reason", [
    data(candidate({"text": "before"}, finish="STOP")),
    data(candidate({"text": "after"})),
    data(candidate({"text": "late"})),
    END,
])
scenario("stream_break_on_blank_finish_reason", [
    data(candidate({"text": "a"}, finish="   ")),
    data(candidate({"text": "b"}, finish="STOP")),
    END,
])
scenario("stream_done_marker_stops_loop", [
    data(candidate({"text": "a"})),
    raw("data: [DONE]"),
    data(candidate({"text": "after done"})),
    END,
])

# ---- errors ---------------------------------------------------------------------------------------------
scenario("stream_error_payload", [
    data({"error": {"code": 429, "status": "RESOURCE_EXHAUSTED", "message": "quota exceeded"}}),
    data(candidate({"text": "late"})),
    END,
])
scenario("stream_error_fallback_message", [
    data({"error": {"status": "INTERNAL"}}),
    END,
], provider="Relay")
scenario("stream_outcome_failed_to_generate", [
    data({"candidates": None, "outcome": "upstream failed to generate the response"}),
    data(candidate({"text": "late"})),
    END,
])
scenario("stream_outcome_other_value", [
    data({"outcome": "some other outcome"}),
    data(candidate({"text": "kept"}, finish="STOP")),
    END,
])
long_junk = '{"candidates": [' + '"' + "x" * 600
scenario("stream_decode_failure_take512", [
    raw("data: " + long_junk),
    data(candidate({"text": "late"})),
    END,
])

# ---- grounding metadata, hosted updates, citations --------------------------------------------------------
GROUNDING = {
    "groundingChunks": [
        {"web": {"uri": "https://a.example/x", "title": "Alpha"}},
        {"web": {"uri": "https://a.example/x", "title": "Duplicate"}},
        {"web": {"uri": "https://b.example/y"}},
    ],
    "groundingSupports": [
        {"segment": {"startIndex": 0, "endIndex": 5, "text": "first"},
         "groundingChunkIndices": [0, 2]},
        {"segment": {"startIndex": 6, "endIndex": 11, "text": "second"},
         "groundingChunkIndices": [1]},
    ],
    "webSearchQueries": ["agora stream", "gemini citations"],
}
scenario("stream_grounding_hosted_once_per_change", [
    data(candidate({"text": "first"}, grounding=GROUNDING)),
    data(candidate({"text": " second"}, grounding=GROUNDING)),
    data(candidate({"text": " third"}, finish="STOP", grounding=GROUNDING)),
    END,
])
scenario("stream_grounding_metadata_changed", [
    data(candidate({"text": "first"}, grounding=GROUNDING)),
    data(candidate({"text": "second"}, finish="STOP", grounding={
        "groundingChunks": [{"web": {"uri": "https://c.example/z"}}],
        "groundingSupports": [],
        "webSearchQueries": ["changed"],
    })),
    END,
])
scenario("stream_grounding_citations_final_pass", [
    data(candidate({"text": "hello"}, grounding=GROUNDING)),
    END,
])

# ---- termination inputs and usage math -----------------------------------------------------------------------
scenario("stream_usage_derived_total", [
    data(usage(promptTokenCount=10, candidatesTokenCount=5, thoughtsTokenCount=3)),
    data(candidate({"text": "done"}, finish="STOP")),
    END,
])
scenario("stream_usage_total_wins_and_coercion", [
    data(usage(promptTokenCount=-3, candidatesTokenCount=-1, thoughtsTokenCount=0,
               totalTokenCount=99)),
    data(candidate({"text": "done"}, finish="STOP")),
    END,
])
scenario("stream_pending_code_execution_in_flight", [
    data(candidate({"executableCode": {"language": "python", "code": "a"}})),
    END,
])
scenario("stream_tool_call_counts_as_produced", [
    data(candidate({"functionCall": {"id": "c1", "name": "lookup", "args": {}}})),
    END,
])


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for item in ALL:
        # The providerName lives next to ops; a None inside data() is handled by the caller.
        ops = item["ops"]
        payload = {"name": item["name"], "providerName": item["providerName"], "ops": ops}
        path = os.path.join(SCENARIOS_DIR, item["name"] + ".json")
        with open(path, "w", encoding="utf-8") as handle:
            json.dump(payload, handle, indent=2, ensure_ascii=False)
            handle.write("\n")
    print(f"Wrote {len(ALL)} scenarios")


if __name__ == "__main__":
    main()
