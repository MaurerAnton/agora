#!/usr/bin/env python3
"""Deterministic scenario generator for ExportExtraSettings.restoreLegacyFromJsonObject.

Each scenario is a JSON document consumed by ExportExtraSettingsTraceDump.kt. `object` is passed
to the Kotlin function unchanged; the remaining fields seed the SettingsManager double and the
replace/allowSecrets/allowedConversationIds arguments.
"""
from __future__ import annotations

import json
import os
import random
import shutil

HERE = os.path.dirname(os.path.abspath(__file__))
OUT = os.path.join(HERE, "scenarios")

SCALARS = {
    "imageTranscriptionEnabled": True,
    "imageTranscriptionEnabledModels": "whisper-large,whisper-small",
    "imageTranscriptionModel": "gpt-4o-mini",
    "imageTranscriptionBatchSize": 4,
    "imageTranscriptionPrompt": "Describe the image.",
    "webSearchNumResults": 5,
    "searchContextWindow": 8192,
    "searchMatchLimit": 12,
    "defaultTemperature": 0.7,
    "defaultMaxTokens": 4096,
    "defaultTopP": 0.9,
    "defaultFrequencyPenalty": 0.1,
    "defaultPresencePenalty": -0.2,
    "showDocumentationFab": True,
    "proxyEnabled": True,
    "proxyType": "http",
    "proxyHost": "proxy.example",
    "proxyPort": "8080",
    "proxyUsername": "user",
    "proxyPassword": "secret",
    "proxyBypass": "localhost,127.0.0.1",
    "themeMode": "dark",
    "colorScheme": "green",
    "dynamicColor": False,
    "blurEffectsEnabled": True,
    "reduceMotion": False,
    "openAiServiceTierEnabled": True,
    "openAiServiceTier": "flex",
    "hapticsEnabled": False,
    "detailedTokenUsage": True,
    "autoExpandActiveGroup": False,
    "schemeStyle": "tonal",
    "fontPreference": "system",
    "autoUpdateCheck": True,
    "automationToolsEnabled": False,
    "exactExecutionEnabled": True,
    "automationWakeLockEnabled": False,
}

CONVERSATION_ALL = {
    "contextWindow": 128000,
    "temperature": 0.6,
    "maxTokens": 2048,
    "topP": 0.95,
    "frequencyPenalty": 0.25,
    "presencePenalty": -0.5,
    "codeExecutionEnabled": True,
    "googleSearchEnabled": False,
    "openAiWebSearchEnabled": True,
    "thinkingEnabled": True,
    "thinkingLevel": "high",
    "thinkingBudgetEnabled": False,
    "thinkingBudgetTokens": 0,
    "openAiServiceTierEnabled": True,
    "openAiServiceTier": "priority",
    "webSearchEnabled": False,
    "shellEnabled": True,
    "lowContextModeEnabled": False,
}


def write(name: str, obj, *, replace=False, allow_secrets=False, allowed=None,
          model_aliases=None, custom_models=None) -> None:
    scenario = {
        "object": obj,
        "replace": replace,
        "allowSecrets": allow_secrets,
        "allowedConversationIds": allowed or [],
        "modelAliases": model_aliases or {},
        "customModels": custom_models or [],
    }
    with open(os.path.join(OUT, name + ".json"), "w", encoding="utf-8") as handle:
        json.dump(scenario, handle, ensure_ascii=False, indent=2)
        handle.write("\n")


def main() -> None:
    shutil.rmtree(OUT, ignore_errors=True)
    os.makedirs(OUT)

    # Empty and scalar-field coverage.
    write("01-empty-object", {})
    write("02-all-scalars", dict(SCALARS))
    write("03-all-scalars-false-zero", {
        **SCALARS,
        "imageTranscriptionEnabled": False,
        "imageTranscriptionEnabledModels": "",
        "imageTranscriptionModel": "",
        "imageTranscriptionBatchSize": 0,
        "imageTranscriptionPrompt": "",
        "webSearchNumResults": -1,
        "searchContextWindow": 0,
        "searchMatchLimit": -7,
        "defaultTemperature": 0.0,
        "defaultMaxTokens": 0,
        "defaultTopP": -0.0,
        "defaultFrequencyPenalty": 0.0,
        "defaultPresencePenalty": 0.0,
        "showDocumentationFab": False,
        "proxyEnabled": False,
        "proxyType": "",
        "proxyHost": "",
        "proxyPort": "",
        "proxyUsername": "",
        "proxyBypass": "",
        "themeMode": "",
        "colorScheme": "",
        "dynamicColor": True,
        "blurEffectsEnabled": False,
        "reduceMotion": True,
        "openAiServiceTierEnabled": False,
        "openAiServiceTier": "",
        "hapticsEnabled": True,
        "detailedTokenUsage": False,
        "autoExpandActiveGroup": True,
        "schemeStyle": "",
        "fontPreference": "",
        "autoUpdateCheck": False,
        "automationToolsEnabled": True,
        "exactExecutionEnabled": False,
        "automationWakeLockEnabled": True,
    })
    write("04-float-spellings", {
        "defaultTemperature": 0.1,
        "defaultMaxTokens": 2147483647,
        "defaultTopP": 1e-7,
        "defaultFrequencyPenalty": -123.456,
        "defaultPresencePenalty": 3.4028235e38,
    })
    write("05-float-overflow", {"defaultTemperature": 1e39, "defaultTopP": -1e39})
    write("06-string-primitives-for-numbers", {
        "imageTranscriptionBatchSize": " 7 ",
        "webSearchNumResults": "+3",
        "searchContextWindow": "00042",
        "searchMatchLimit": "-0",
        "defaultTemperature": " 0.5f ",
        "defaultMaxTokens": "12",
        "defaultTopP": "NaN",
        "defaultFrequencyPenalty": "Infinity",
        "defaultPresencePenalty": "-Infinity",
    })
    write("07-content-or-null-primitives", {
        "imageTranscriptionModel": 42,
        "imageTranscriptionPrompt": True,
        "proxyType": 3.5,
        "proxyHost": False,
        "themeMode": None,
        "colorScheme": ["not", "a", "primitive"],
    })

    # Primitive spellings. Each value is an independent scenario so a failure cannot hide the
    # behaviour of the values after it.
    int_spellings = [
        " 7 ", "+3", " +3 ", "-3", "00042", "3e2", "3E+2", "3e-1", "10e-1", "3e",
        "3e+", "3e-", "3e+2x", "1e10", "-2147483648", "2147483647", "2147483648",
        "0x10", "1_000", "3.0", "\t7\n", "\u00a03\u00a0", "\u20033\u2003",
    ]
    for index, value in enumerate(int_spellings):
        write(f"08-int-spelling-{index:02d}", {"webSearchNumResults": value})

    float_spellings = [
        "0.5", " 0.5 ", "0.5f", "0.5F", "0.5d", "0.5D", ".5", "5.", "+.5", "-.5e1",
        "0x1.8p1", "0x10", "0x1p2f", "1e39", "1e-50", "NaN", "NAN", "nan", "+NaN",
        "-NaN", "Infinity", "+Infinity", "-Infinity", "infinity", "-infinity",
        "1,5", "", "1_0", "0x1.8",
    ]
    for index, value in enumerate(float_spellings):
        write(f"09-float-spelling-{index:02d}", {"defaultTemperature": value})

    bool_spellings = ["true", "false", "TRUE", "False", "yes", " true ", ""]
    for index, value in enumerate(bool_spellings):
        write(f"11-bool-spelling-{index:02d}", {"imageTranscriptionEnabled": value})

    # Image model list splitting follows Kotlin split(",") + isNotBlank() without trimming.
    for index, value in enumerate(["", "a", "a,b", ",a,,b,", " a , b ", "a,, ,b", ",",
                                   "ü,日本,🙂", "a,b,a"]):
        write(f"10-image-models-{index:02d}", {"imageTranscriptionEnabledModels": value})

    # Proxy secret gating.
    write("20-proxy-secret-allowed", {"proxyPassword": "s3cr3t"}, allow_secrets=True)
    write("21-proxy-secret-denied", {"proxyPassword": "s3cr3t"}, allow_secrets=False)
    write("22-proxy-secret-empty", {"proxyPassword": ""}, allow_secrets=True)
    write("23-proxy-bypass-filter", {"proxyBypass": " "}, allow_secrets=True)
    write("24-proxy-bypass-empty", {"proxyBypass": ""})
    write("25-proxy-no-field", {}, allow_secrets=True)

    # Conversation settings.
    write("30-conversation-all-fields", {"conversationSettings": {"c1": CONVERSATION_ALL}},
          allowed=["c1"])
    write("31-conversation-filter", {"conversationSettings": {
        "c1": {"contextWindow": 1},
        "c2": {"contextWindow": 2},
        "c3": {"contextWindow": 3},
    }}, allowed=["c3", "c1"])
    write("32-conversation-empty", {"conversationSettings": {}}, allowed=["c1"])
    write("33-conversation-all-null", {"conversationSettings": {"c1": {
        key: None for key in CONVERSATION_ALL
    }}}, allowed=["c1"])
    write("34-conversation-unknown-fields", {"conversationSettings": {"c1": {
        "unknown": 1, "another": [2], "contextWindow": 99
    }}}, allowed=["c1"])
    write("35-conversation-empty-id", {"conversationSettings": {"": {"maxTokens": 5}}},
          allowed=[""])
    write("36-conversation-unicode-id", {"conversationSettings": {"беседа-🙂": {
        "shellEnabled": True
    }}}, allowed=["беседа-🙂"])
    write("37-conversation-value-not-object", {"conversationSettings": {"c1": []}},
          allowed=["c1"])
    write("38-conversation-not-object", {"conversationSettings": []}, allowed=["c1"])

    # Legacy thinking budgets and effort normalisation.
    budget_cases = {
        "budget:2048": {"thinkingLevel": "budget:2048"},
        "budget:0": {"thinkingLevel": "budget:0"},
        "budget:-5": {"thinkingLevel": "budget:-5"},
        "budget:abc": {"thinkingLevel": "budget:abc"},
        "budget: 42": {"thinkingLevel": "budget: 42"},
        "budget:+7": {"thinkingLevel": "budget:+7"},
        "budget-overflow": {"thinkingLevel": "budget:99999999999"},
        "budget-upper": {"thinkingLevel": "BUDGET:7"},
        "budget-spaced": {"thinkingLevel": " budget: 8 "},
        "budget-explicit-disabled": {
            "thinkingLevel": "budget:2048", "thinkingBudgetEnabled": False,
            "thinkingBudgetTokens": 0,
        },
        "budget-explicit-enabled": {
            "thinkingLevel": "budget:2048", "thinkingBudgetEnabled": True,
            "thinkingBudgetTokens": 1024,
        },
        "budget-explicit-tokens-only": {
            "thinkingLevel": "budget:2048", "thinkingBudgetTokens": 512,
        },
        "effort-alias-min": {"thinkingLevel": "MIN"},
        "effort-alias-balanced": {"thinkingLevel": "balanced"},
        "effort-alias-extra-high": {"thinkingLevel": "extra-high"},
        "effort-alias-quick": {"thinkingLevel": "quick"},
        "effort-unknown": {"thinkingLevel": "turbo"},
        "effort-empty": {"thinkingLevel": ""},
        "effort-none": {"thinkingLevel": "none"},
    }
    for index, (case, settings) in enumerate(budget_cases.items()):
        write(f"40-thinking-{index:02d}-{case}", {
            "conversationSettings": {"c1": settings}
        }, allowed=["c1"])

    tier_cases = {
        "scale": "scale", "priority": "priority", "upper": " FAST ", "unknown": "bogus",
        "empty": "", "auto": "AUTO", "ultrafast": "ultraFast", "flex": " flex ",
        "null": None, "number": 3,
    }
    for index, (case, tier) in enumerate(tier_cases.items()):
        write(f"50-tier-{index:02d}-{case}", {
            "conversationSettings": {"c1": {"openAiServiceTier": tier}}
        }, allowed=["c1"])

    # Conversation primitives accepted from strings and numbers.
    write("55-conversation-primitive-strings", {"conversationSettings": {"c1": {
        "contextWindow": "2048",
        "temperature": "0.25",
        "maxTokens": 17,
        "topP": 1,
        "frequencyPenalty": "0.125",
        "presencePenalty": "-0.0",
        "codeExecutionEnabled": "TRUE",
        "googleSearchEnabled": "false",
        "openAiWebSearchEnabled": 1,
        "thinkingEnabled": 0,
        "thinkingLevel": 123,
        "thinkingBudgetEnabled": "true",
        "thinkingBudgetTokens": "33",
        "openAiServiceTierEnabled": "False",
        "openAiServiceTier": 5,
        "webSearchEnabled": " true ",
        "shellEnabled": "no",
        "lowContextModeEnabled": None,
    }}}, allowed=["c1"])

    # modelAliases merge/replace and primitive filtering.
    alias_map = {"m1": "One", "m2": "Two", "m3": "Three"}
    for replace in (False, True):
        write(f"60-aliases-replace-{int(replace)}",
              {"modelAliases": alias_map},
              replace=replace,
              model_aliases={"seed1": "Seed", "m2": "Old"})
    write("61-aliases-empty", {"modelAliases": {}}, model_aliases={"seed": "Seed"})
    write("62-aliases-primitives", {"modelAliases": {
        "s": "text", "n": 42, "b": True, "z": None, "f": 1.5
    }}, model_aliases={"seed": "Seed"})
    write("63-aliases-only-null", {"modelAliases": {"a": None}},
          model_aliases={"seed": "Seed"})
    write("64-aliases-value-object", {"modelAliases": {"a": {"nested": 1}}})
    write("65-aliases-not-object", {"modelAliases": []})
    write("66-aliases-unicode", {"modelAliases": {"модель": "🙂", "": "empty"}},
          replace=True)

    # customModels merge/replace and set semantics.
    for replace in (False, True):
        write(f"70-custom-replace-{int(replace)}",
              {"customModels": ["m3", "m1", "m3"]},
              replace=replace,
              custom_models=["seed", "m1"])
    write("71-custom-empty-replace", {"customModels": []}, replace=True,
          custom_models=["seed"])
    write("72-custom-empty-merge", {"customModels": []}, replace=False,
          custom_models=["seed", "seed2"])
    write("73-custom-primitives", {"customModels": ["s", 42, True, 1.5, None]},
          custom_models=["seed"])
    write("74-custom-element-object", {"customModels": [{"x": 1}]})
    write("75-custom-not-array-object", {"customModels": {"a": "b"}},
          custom_models=["seed"])
    write("76-custom-not-array-string", {"customModels": "m1"}, replace=True,
          custom_models=["seed"])

    # Scalar type failures: nulls, objects, arrays and unparsable strings.
    error_cases = {
        "80-bool-null": {"imageTranscriptionEnabled": None},
        "81-bool-number": {"imageTranscriptionEnabled": 1},
        "82-bool-word": {"imageTranscriptionEnabled": "yes"},
        "83-bool-object": {"imageTranscriptionEnabled": {}},
        "84-int-null": {"imageTranscriptionBatchSize": None},
        "85-int-decimal": {"imageTranscriptionBatchSize": 1.5},
        "86-int-junk": {"imageTranscriptionBatchSize": "12x"},
        "87-int-overflow": {"imageTranscriptionBatchSize": "2147483648"},
        "88-int-array": {"webSearchNumResults": []},
        "89-float-null": {"defaultTemperature": None},
        "90-float-junk": {"defaultTemperature": "0.5.5"},
        "91-float-array": {"defaultTopP": []},
        "92-string-object": {"imageTranscriptionModel": {}},
        "93-int-hex": {"searchContextWindow": "0x10"},
        "94-int-underscore": {"searchMatchLimit": "1_000"},
    }
    for name, obj in error_cases.items():
        write(name, obj)

    # Partial trace before an error: earlier writes must survive.
    write("95-partial-before-error", {
        "imageTranscriptionEnabled": True,
        "imageTranscriptionModel": "m",
        "conversationSettings": {"c1": {"contextWindow": 1}},
        "imageTranscriptionBatchSize": None,
        "themeMode": "dark",
    }, allowed=["c1"])

    # Deterministic mixed-field fuzz corpus.
    keys = list(SCALARS) + ["conversationSettings", "modelAliases", "customModels"]
    valid_values = {
        **SCALARS,
        "conversationSettings": {"c1": CONVERSATION_ALL, "c2": {"maxTokens": 9}},
        "modelAliases": {"a": "A", "b": "B"},
        "customModels": ["x", "y", "x"],
    }
    invalid_values = [
        None, [], {}, True, False, 0, 1, -1, 1.5, "", " ", "text", "true", "false",
        "budget:64", "scale", 2147483648,
    ]
    for index in range(12):
        rng = random.Random(1000 + index)
        chosen = rng.sample(keys, rng.randint(5, 12))
        obj = {}
        for key in chosen:
            if rng.random() < 0.72:
                obj[key] = valid_values[key]
            else:
                obj[key] = rng.choice(invalid_values)
        write(f"100-fuzz-{index:02d}", obj,
              replace=rng.choice([False, True]),
              allow_secrets=rng.choice([False, True]),
              allowed=["c1"] if rng.random() < 0.6 else [],
              model_aliases={"seed": "S"},
              custom_models=["seed"])

    print(f"Wrote {len(os.listdir(OUT))} scenarios to {OUT}")


if __name__ == "__main__":
    main()
