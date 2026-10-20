#!/usr/bin/env python3
"""Generates deterministic scenarios for ModelThinkingCapabilityResolver.

Covers every branch of ModelId.parse, all custom endpoint protocols (including wire aliases),
match by display name / stable id / legacy name, first-match order, case sensitivity, empty and
multi-colon prefixes, unknown providers, and every thinking-capability family.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")
CASES = []


def case(name, selected, providers=None):
    CASES.append({
        "name": name,
        "selectedModel": selected,
        "customProviders": providers or [],
    })


def provider(name, protocol="openai", id="", legacy=None):
    return {
        "name": name,
        "protocol": protocol,
        "id": id,
        "legacyNames": legacy or [],
    }


# ModelId.parse: unprefixed legacy heuristics.
for name, model in [
    ("legacy_gpt", "gpt-4o"),
    ("legacy_o1", "o1-preview"),
    ("legacy_o3", "o3-mini"),
    ("legacy_o4", "o4-mini"),
    ("legacy_o1_without_dash", "o100"),
    ("legacy_claude", "claude-3-opus"),
    ("legacy_deepseek", "deepseek-chat"),
    ("legacy_qwen", "qwen2.5-coder"),
    ("legacy_groq", "groq/llama-3"),
    ("legacy_google_models", "models/gemini-2.5-pro"),
    ("legacy_google_gemini", "gemini-1.5-flash"),
    ("legacy_unknown", "mistral-large"),
    ("legacy_uppercase_is_not_a_prefix", "GPT-4o"),
]:
    case(name, model)

case("prefixed_openai", "OpenAI:gpt-4o")
case("prefixed_empty_provider", ":gpt-4o")
case("prefixed_empty_model", "OpenAI:")
case("prefixed_first_colon_wins", "Provider:model:with:colons")
case("prefixed_model_contains_colon", "Custom:org/model:tag")

# Custom provider matching and protocol override.
case("custom_name_match_anthropic", "Relay:claude-3-opus", [
    provider("Relay", "anthropic"),
])
case("custom_name_match_google", "Relay:gemini-2.5-pro", [
    provider("Relay", "google"),
])
case("custom_stable_id_match", "custom-provider-12345678-1234-4234-9234-123456789abc:gpt-4o", [
    provider("Renamed Relay", "anthropic",
             "custom-provider-12345678-1234-4234-9234-123456789abc", ["Old Relay"]),
])
case("custom_legacy_name_match", "Old Relay:gpt-4o", [
    provider("New Relay", "google",
             "custom-provider-12345678-1234-4234-9234-123456789abc", ["Old Relay"]),
])
case("custom_stable_id_case_insensitive_test", "CUSTOM-PROVIDER-ABCDEFAB-1234-4234-9234-123456789ABC:gpt-4o", [
    provider("Relay", "anthropic",
             "CUSTOM-PROVIDER-ABCDEFAB-1234-4234-9234-123456789ABC"),
])
case("custom_stable_id_with_spaces_is_still_stable", "  custom-provider-12345678-1234-4234-9234-123456789abc  :gpt-4o", [
    provider("Relay", "openai", "  custom-provider-12345678-1234-4234-9234-123456789abc  "),
])
case("custom_first_match_wins", "Relay:gpt-4o", [
    provider("Relay", "anthropic"),
    provider("Relay", "google"),
])
case("custom_name_is_case_sensitive", "relay:gpt-4o", [
    provider("Relay", "anthropic"),
])
case("custom_name_does_not_match_builtin", "OpenAI:gpt-4o", [
    provider("Other", "anthropic"),
])
case("custom_unknown_wire_protocol", "Relay:gpt-4o", [
    provider("Relay", "something-else"),
])
case("custom_protocol_gemini_alias", "Relay:gpt-4o", [
    provider("Relay", "gemini"),
])
case("custom_protocol_claude_alias", "Relay:gpt-4o", [
    provider("Relay", "claude"),
])
case("custom_protocol_case_and_whitespace", "Relay:gpt-4o", [
    provider("Relay", "  AnThRoPiC  "),
])
case("custom_blank_protocol", "Relay:gpt-4o", [
    provider("Relay", "   "),
])
case("unknown_provider_name_can_match_custom", "Unknown:model", [
    provider("Unknown", "google"),
])
case("custom_legacy_names_order", "Alias B:gpt-4o", [
    provider("Relay", "anthropic",
             "custom-provider-12345678-1234-4234-9234-123456789abc",
             ["Alias A", "Alias B", "Alias C"]),
])
case("custom_empty_id_uses_name", "Relay:claude-3", [
    provider("Relay", "openai", ""),
])
case("custom_provider_without_match", "Elsewhere:claude-3", [
    provider("Relay", "google"),
])


def main():
    os.makedirs(SCENARIOS, exist_ok=True)
    for old in os.listdir(SCENARIOS):
        if old.endswith(".json"):
            os.remove(os.path.join(SCENARIOS, old))
    for entry in CASES:
        with open(os.path.join(SCENARIOS, entry["name"] + ".json"), "w", encoding="utf-8") as handle:
            json.dump(entry, handle, ensure_ascii=False, indent=2)
            handle.write("\n")
    print(f"wrote {len(CASES)} scenarios to {SCENARIOS}")


if __name__ == "__main__":
    main()
