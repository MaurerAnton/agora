#!/usr/bin/env python3
"""Generates deterministic scenarios for the CustomProviderConfig provider-protocol helpers."""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS = os.path.join(HERE, "scenarios")
CASES = []


def case(name, provider, providers=None, **fields):
    CASES.append({
        "name": name,
        "providerName": provider,
        "customProviders": providers or [],
        **fields,
    })


def provider(name, protocol="openai", id="", legacy=None, responses=False, cache=True, ttl="1h"):
    return {
        "name": name,
        "protocol": protocol,
        "id": id,
        "legacyNames": legacy or [],
        "responsesApiEnabled": responses,
        "anthropicCacheEnabled": cache,
        "anthropicCacheTtl": ttl,
    }


STABLE = "custom-provider-12345678-1234-4234-9234-123456789abc"

# Built-in names: no custom providers needed.
case("builtin_openai_defaults", "OpenAI")
case("builtin_anthropic_defaults", "Anthropic")
case("builtin_openai_disabled", "OpenAI", builtInEnabled=False)
case("builtin_anthropic_disabled", "Anthropic", builtInAnthropicEnabled=False,
     builtInTtl="5m")
case("builtin_anthropic_cache_disabled", "Anthropic", builtInEnabled=False)
case("builtin_other_no_custom", "Other")

# Custom provider protocol detection.
case("custom_openai_by_name", "Relay", [provider("Relay", "openai")])
case("custom_anthropic_by_name", "Relay", [provider("Relay", "anthropic")])
case("custom_google_by_name", "Relay", [provider("Relay", "google")])
case("custom_unknown_by_name", "Relay", [provider("Relay", "unknown")])
case("custom_openai_by_stable_id", STABLE, [provider("Renamed", "openai", STABLE)])
case("custom_anthropic_by_legacy", "Old Name", [
    provider("New Name", "anthropic", STABLE, ["Old Name"]),
])

# Cache switches and TTL.
case("custom_anthropic_cache_disabled", "Relay", [
    provider("Relay", "anthropic", cache=False, ttl="5m"),
])
case("custom_anthropic_cache_enabled_ttl", "Relay", [
    provider("Relay", "anthropic", cache=True, ttl="5m"),
])
case("custom_openai_cache_disabled", "Relay", [
    provider("Relay", "openai", cache=False, responses=True),
])
case("custom_openai_responses_enabled", "Relay", [
    provider("Relay", "openai", cache=True, responses=True),
])
case("custom_google_switches_ignored", "Relay", [
    provider("Relay", "google", cache=False, responses=True),
])
case("custom_unknown_cache_switch_still_reported", "Relay", [
    provider("Relay", "unknown", cache=False, ttl="9m"),
])

# firstOrNull vs any: the first name match owns cache/TTL/responses; any match can satisfy the
# protocol predicates.
case("first_match_is_anthropic_then_openai", "Relay", [
    provider("Relay", "anthropic", cache=False, ttl="5m"),
    provider("Relay", "openai", responses=True),
])
case("first_match_is_google_then_anthropic", "Relay", [
    provider("Relay", "google", cache=False, ttl="9m"),
    provider("Relay", "anthropic", responses=True),
])
case("first_match_is_legacy_then_name", "Old Name", [
    provider("First", "anthropic", STABLE, ["Old Name"], cache=False),
    provider("Old Name", "openai", responses=True),
])
case("any_openai_after_nonmatching", "Relay", [
    provider("Other", "anthropic"),
    provider("Relay", "openai", responses=True),
])
case("any_anthropic_after_nonmatching", "Relay", [
    provider("Other", "openai"),
    provider("Relay", "anthropic"),
])

# Built-in name shadows custom providers in the when branches, but firstOrNull still decides the
# custom cache/TTL/responses fallback.
case("builtin_openai_name_with_custom_google", "OpenAI", [
    provider("OpenAI", "google", cache=False, ttl="9m", responses=True),
])
case("builtin_anthropic_name_with_custom_google", "Anthropic", [
    provider("Anthropic", "google", cache=False, ttl="9m"),
], builtInAnthropicEnabled=False, builtInTtl="30m")
case("custom_name_case_sensitive", "relay", [provider("Relay", "openai")])
case("custom_empty_id_uses_name", "Relay", [provider("Relay", "anthropic", "")])
case("custom_legacy_names_order", "Alias B", [
    provider("Relay", "anthropic", STABLE, ["Alias C", "Alias B"], cache=False),
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
