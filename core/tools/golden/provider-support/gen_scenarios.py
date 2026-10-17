#!/usr/bin/env python3
"""Generates the scenario corpus for the provider-support golden stand.

Deterministic: rerunning this script rewrites the same scenarios/*.json. Freeze the golden traces
afterwards with run_golden.sh --freeze.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def http_error(name, status, body):
    ALL.append({"name": name, "mode": "httpError", "statusCode": status, "rawBody": body})


def base_url(name, url):
    ALL.append({"name": name, "mode": "baseUrl", "url": url})


def defaults(name, embedding="", compatible="", openai_map=None):
    ALL.append({
        "name": name,
        "mode": "defaults",
        "embeddingProvider": embedding,
        "compatibleName": compatible,
        "openAiMap": openai_map or {},
    })


# ---- HTTP error bodies --------------------------------------------------------------------------
http_error("http_null_body", 404, None)
http_error("http_blank_body", 404, "   ")
http_error("http_free_text", 503, "upstream exploded")
http_error("http_primitive_string", 429, '"quota exceeded"')
http_error("http_primitive_blank_string", 500, '"   "')
http_error("http_primitive_number", 502, "42")
http_error("http_primitive_true", 500, "true")
http_error("http_json_null_literal", 500, "null")
http_error("http_array_root", 500, "[1, 2]")
http_error("http_object_nested_full", 429,
           '{"error": {"code": "rate_limit", "type": "overloaded", "message": "slow down"}}')
http_error("http_object_nested_status_detail", 401,
           '{"error": {"status": "InvalidApiKey", "detail": "bad key"}}')
http_error("http_object_error_string", 500, '{"error": "quota hit"}')
http_error("http_object_root_message", 500, '{"message": "root msg"}')
http_error("http_object_root_detail", 500, '{"detail": "root detail"}')
http_error("http_object_root_reason", 500, '{"reason": "root reason"}')
http_error("http_object_root_error_description", 500, '{"error_description": "oauth failed"}')
http_error("http_object_no_message", 500, '{"code": 500}')
http_error("http_nested_number_code", 429, '{"error": {"code": 429, "message": "rl"}}')
http_error("http_nested_trimmed_message", 500, '{"error": {"message": "  padded  "}}')
http_error("http_nested_null_message_falls_back", 500,
           '{"error": {"message": null, "detail": "detail wins"}}')
http_error("http_status_fallback_code", 502, '{"error": {"message": "bad gateway"}}')
http_error("http_quoted_null_string_message", 500, '{"error": "null"}')

# ---- base URL versioning --------------------------------------------------------------------------
base_url("base_url_plain", "https://api.example.com")
base_url("base_url_blank", "")
base_url("base_url_slash_only", "/")
base_url("base_url_version", "https://api.example.com/v1")
base_url("base_url_version_trailing_slash", "https://api.example.com/v1/")
base_url("base_url_version_multislash", "https://api.example.com/v1///")
base_url("base_url_version_beta", "https://api.example.com/v1beta")
base_url("base_url_version_midpath", "https://api.example.com/v1/proxy")
base_url("base_url_version_v2", "https://api.example.com/v2")
base_url("base_url_version_upper", "https://api.example.com/V1BETA")
base_url("base_url_no_slash_prefix", "v1")
base_url("base_url_only_version", "/v1")
base_url("base_url_double_slash_v", "https://v1.example.com/x")
base_url("base_url_version_trailing_newline", "https://api.example.com/v1beta\n")
base_url("base_url_digit_run", "https://api.example.com/v1x_9")

# ---- provider defaults -----------------------------------------------------------------------------
defaults("defaults_openai", embedding="OpenAI", compatible="OpenAI",
         openai_map={"OpenAI": "https://custom.openai/v1"})
defaults("defaults_openai_empty_map", embedding="openai", compatible="openai", openai_map={})
defaults("defaults_openai_empty_value", embedding="", compatible="OpenAI",
         openai_map={"OpenAI": ""})
defaults("defaults_openai_other_key", embedding="", compatible="Google",
         openai_map={"Google": "https://elsewhere/v1"})
defaults("defaults_open_router", embedding="open router", compatible="Open Router")
defaults("defaults_openrouter_compact", embedding="OpenRouter")
defaults("defaults_mistral_upper", embedding="MISTRAL")
defaults("defaults_voyage_variants", embedding="voyage ai")
defaults("defaults_voyage_compact", embedding="VOYAGE")
defaults("defaults_siliconflow", embedding="siliconflow")
defaults("defaults_ollama", embedding="ollama")
defaults("defaults_unknown", embedding="deepseek")


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
