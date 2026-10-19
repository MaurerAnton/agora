#!/usr/bin/env python3
"""Generates the openai-service-tiers scenarios. Deterministic; rerun after editing.

Six operations mirror the six public functions of model/OpenAiServiceTiers.kt: `normalize`,
`available`, `mapped`, `index`, `tier`, `request`. Null strings serialize as JSON null (the
driver prints `-`); omitted booleans fall back to the Kotlin defaults.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def normalize(name, value):
    case(name, op="normalize", value=value)


def available(name, model, official=True):
    case(name, op="available", model=model, official=official)


def mapped(name, value, model, official=True):
    case(name, op="mapped", value=value, model=model, official=official)


def index(name, value):
    case(name, op="index", value=value)


def tier(name, index_value):
    case(name, op="tier", index=index_value)


def request(name, enabled, value, responses, model=None, official=True):
    case(
        name,
        op="request",
        enabled=enabled,
        value=value,
        responses=responses,
        model=model,
        official=official,
    )


# ---- normalize -------------------------------------------------------------------------------
normalize("normalize_null", None)
normalize("normalize_empty", "")
normalize("normalize_blank", "   ")
normalize("normalize_auto", "auto")
normalize("normalize_default", "default")
normalize("normalize_flex", "flex")
normalize("normalize_fast", "fast")
normalize("normalize_ultrafast", "ultrafast")
normalize("normalize_uppercase", "DEFAULT")
normalize("normalize_padded", " Flex ")
normalize("normalize_tabs", "\tFAST\n")
normalize("normalize_nbsp_padded", "\u00a0flex\u00a0")
normalize("normalize_scale_alias", "scale")
normalize("normalize_scale_alias_upper", " SCALE ")
normalize("normalize_priority_alias", "priority")
normalize("normalize_priority_alias_mixed", "PRIORITY")
normalize("normalize_unknown", "bogus")
normalize("normalize_partial", "flexx")

# ---- availableTiers --------------------------------------------------------------------------
available("available_all_null", None)
available("available_empty_model", "")
available("available_gpt54", "gpt-5.4")
available("available_gpt54mini", "gpt-5.4-mini")
available("available_gpt41mini", "gpt-4.1-mini")
available("available_o3pro", "o3-pro")
available("available_gpt55pro", "gpt-5.5-pro")
available("available_gpt4o", "gpt-4o")
available("available_gpt52pro", "gpt-5.2-pro")
available("available_gpt5pro", "gpt-5-pro")
available("available_gpt56sol", "gpt-5.6-sol")
available("available_gpt56luna", "gpt-5.6-luna")
available("available_codex", "gpt-5.3-codex")
available("available_o3", "o3")
available("available_o4mini", "o4-mini")
available("available_gpt41", "gpt-4.1")
available("available_unknown", "gpt-x")
available("available_case_padded", " GPT-5-PRO ")
available("available_gpt6astra", "GPT-6-ASTRA")
available("available_o1pro", "o1-pro")
available("available_davinci", "davinci-002")
available("available_custom_provider", "gpt-5-pro", official=False)
available("available_custom_provider_unknown", "whatever", official=False)

# ---- mappedTier ------------------------------------------------------------------------------
mapped("mapped_null", None, "gpt-5-pro")
mapped("mapped_auto", "auto", "gpt-5-pro")
mapped("mapped_default", "default", "gpt-5-pro")
mapped("mapped_flex_supported", "flex", "gpt-5.5")
mapped("mapped_flex_unsupported", "flex", "gpt-4o")
mapped("mapped_fast_supported", "fast", "gpt-4o")
mapped("mapped_fast_unsupported", "fast", "gpt-5-pro")
mapped("mapped_ultrafast_supported", "ultrafast", "gpt-5.6-sol")
mapped("mapped_ultrafast_to_fast_luna", "ultrafast", "gpt-5.6-luna")
mapped("mapped_ultrafast_to_fast_54", "ultrafast", "gpt-5.4")
mapped("mapped_ultrafast_unknown_model", "ultrafast", "unknown-model")
mapped("mapped_fast_on_sol", "fast", "gpt-5.6-sol")
mapped("mapped_priority_gpt5pro", "PRIORITY", "gpt-5-pro")
mapped("mapped_priority_gpt4o", "PRIORITY", "gpt-4o")
mapped("mapped_scale", "scale", "gpt-5-pro")
mapped("mapped_flex_custom_provider", "flex", "gpt-5-pro", official=False)
mapped("mapped_ultrafast_custom_provider", "ultrafast", "gpt-5-pro", official=False)
mapped("mapped_flex_gpt56sol", "flex", "gpt-5.6-sol")
mapped("mapped_fast_standard_model", "fast", "gpt-3.5-turbo")
mapped("mapped_padded_value", " Fast ", "gpt-5-pro")
mapped("mapped_null_model", "flex", None)

# ---- indexForTier ----------------------------------------------------------------------------
index("index_null", None)
index("index_empty", "")
index("index_auto", "auto")
index("index_default", "default")
index("index_flex", "flex")
index("index_fast", "fast")
index("index_ultrafast", "ultrafast")
index("index_priority", "PRIORITY")
index("index_scale", "SCALE")
index("index_unknown", "nope")
index("index_padded", " FLEX ")
index("index_uppercase", "FLEX")

# ---- tierForIndex ----------------------------------------------------------------------------
tier("tier_negative", -5)
tier("tier_zero", 0)
tier("tier_one", 1)
tier("tier_two", 2)
tier("tier_three", 3)
tier("tier_four", 4)
tier("tier_large", 99)

# ---- requestValue ----------------------------------------------------------------------------
request("request_disabled", False, "flex", True, "gpt-5.5")
request("request_responses_off", True, "flex", False, "gpt-5.5")
request("request_enabled_flex", True, "flex", True, "gpt-5.5")
request("request_mapped_ultrafast", True, "ultrafast", True, "gpt-5.4")
request("request_null_value", True, None, True, "gpt-5.4")
request("request_both_off", False, "ultrafast", False, "gpt-5.6-sol")
request("request_flex_standard_model", True, "flex", True, "gpt-5-pro")
request("request_custom_provider", True, "flex", True, "gpt-5-pro", official=False)
request("request_priority_gpt4o", True, "PRIORITY", True, "gpt-4o")
request("request_disabled_but_null_model", False, "fast", True, None)
request("request_enabled_ultrafast_sol", True, "ultrafast", True, "gpt-5.6-sol")


def main():
    os.makedirs(SCENARIOS_DIR, exist_ok=True)
    for existing in os.listdir(SCENARIOS_DIR):
        if existing.endswith(".json"):
            os.remove(os.path.join(SCENARIOS_DIR, existing))
    for scenario in ALL:
        path = os.path.join(SCENARIOS_DIR, scenario["name"] + ".json")
        with open(path, "w", encoding="utf-8") as stream:
            json.dump(scenario, stream, ensure_ascii=False, separators=(",", ":"))
            stream.write("\n")
    print(f"wrote {len(ALL)} scenarios to {SCENARIOS_DIR}")


if __name__ == "__main__":
    main()
