#!/usr/bin/env python3
"""Generates the thinking-capability scenarios. Deterministic; rerun after editing.

Every scenario is one operation over the zero-import Kotlin cluster:
`ModelThinkingCapability.kt`, `ThinkingControl.kt`, `ModelThinkingCapabilityDefaults.kt` and
`ModelThinkingCapabilities.kt`. The flows cover the normalization vocabulary, the family/protocol
resolution, every documented per-model refinement, the nearest-effort tie rule, budget clamping and
the two-barrier thinking resolution.
"""
import json
import os

HERE = os.path.dirname(os.path.abspath(__file__))
SCENARIOS_DIR = os.path.join(HERE, "scenarios")

ALL = []


def case(name, **fields):
    ALL.append({"name": name, **fields})


def cap(name, family, model):
    case(name, op="capability", family=family, modelId=model)


def nearest(name, family, model, effort):
    case(name, op="nearest", family=family, modelId=model, effort=effort)


def clamp(name, family, model, tokens):
    case(name, op="clamp", family=family, modelId=model, tokens=tokens)


def resolve(name, family, model, enabled, effort, budget_enabled=False, budget_tokens=0):
    case(name, op="resolve", family=family, modelId=model, enabled=enabled,
         effort=effort, budgetEnabled=budget_enabled, budgetTokens=budget_tokens)


# ---- ThinkingLevels.normalize -----------------------------------------------------------------
case("normalize_null", op="normalize", raw=None)
case("normalize_blank", op="normalize", raw="   ")
case("normalize_nbsp", op="normalize", raw="\u00a0\u00a0")
case("normalize_auto", op="normalize", raw="auto")
case("normalize_budget_dynamic", op="normalize", raw="budget:dynamic")
case("normalize_budget_legacy", op="normalize", raw="budget:8192")
case("normalize_alias_xhigh", op="normalize", raw="  X-High ")
case("normalize_alias_fast", op="normalize", raw="FAST")
case("normalize_none", op="normalize", raw="NONE")
case("normalize_unknown", op="normalize", raw="ultra")
case("normalize_alias_balance", op="normalize", raw="Balanced")

# ---- ThinkingLevels.legacyBudgetTokens --------------------------------------------------------
case("legacy_budget_null", op="legacyBudget", raw=None)
case("legacy_budget_plain", op="legacyBudget", raw="budget:2048")
case("legacy_budget_case_space", op="legacyBudget", raw=" BUDGET:4096 ")
case("legacy_budget_zero", op="legacyBudget", raw="budget:0")
case("legacy_budget_negative", op="legacyBudget", raw="budget:-3")
case("legacy_budget_text", op="legacyBudget", raw="budget:abc")
case("legacy_budget_inner_space", op="legacyBudget", raw="budget: 2048")
case("legacy_budget_empty", op="legacyBudget", raw="budget:")

# ---- ThinkingLevels.effortForIndex / indexForEffort -------------------------------------------
case("effort_for_index_negative", op="effortForIndex", index=-1)
case("effort_for_index_first", op="effortForIndex", index=0)
case("effort_for_index_middle", op="effortForIndex", index=3)
case("effort_for_index_last", op="effortForIndex", index=5)
case("effort_for_index_past_end", op="effortForIndex", index=6)
case("index_for_effort_minimal", op="indexForEffort", effort="minimal")
case("index_for_effort_alias_min", op="indexForEffort", effort="MIN")
case("index_for_effort_none", op="indexForEffort", effort="none")
case("index_for_effort_auto", op="indexForEffort", effort="auto")
case("index_for_effort_alias_xhigh", op="indexForEffort", effort="x-high")
case("index_for_effort_trim_case", op="indexForEffort", effort="  HIGH ")
case("index_for_effort_alias_maximum", op="indexForEffort", effort="maximum")
case("index_for_effort_unknown", op="indexForEffort", effort="bogus")

# ---- ModelThinkingCapabilities.familyFor ------------------------------------------------------
case("family_null", op="family", providerName=None, customProtocol=None)
case("family_anthropic", op="family", providerName="anthropic")
case("family_trim_case", op="family", providerName="  Anthropic ")
case("family_open_router", op="family", providerName="open router")
case("family_openrouter", op="family", providerName="openrouter")
case("family_deepseek", op="family", providerName="deepseek")
case("family_qwen", op="family", providerName="qwen")
case("family_groq", op="family", providerName="groq")
case("family_google", op="family", providerName="google")
case("family_gemini", op="family", providerName="gemini")
case("family_ollama", op="family", providerName="ollama")
case("family_local", op="family", providerName="local")
case("family_unknown", op="family", providerName="unknown")
case("family_protocol_wins", op="family", providerName="anthropic", customProtocol="google")
case("family_protocol_claude", op="family", providerName="openai", customProtocol=" claude ")
case("family_protocol_unknown", op="family", providerName="deepseek", customProtocol="wat")
case("family_protocol_openai_is_compatible", op="family", providerName="anthropic",
     customProtocol="openai")
case("family_protocol_blank_falls_back", op="family", providerName="qwen", customProtocol="   ")

# ---- ModelThinkingCapabilityDefaults.familyDefault --------------------------------------------
for family in ("ANTHROPIC", "OPENAI", "OPENAI_COMPATIBLE", "OPENROUTER", "DEEPSEEK", "QWEN",
               "GROQ", "GEMINI", "OLLAMA", "LOCAL"):
    case("family_default_" + family.lower(), op="familyDefault", family=family)

# ---- ModelThinkingCapabilityDefaults.forModel -------------------------------------------------
cap("cap_anthropic_legacy3", "ANTHROPIC", "claude-3-opus-20240229")
cap("cap_anthropic_legacy35", "ANTHROPIC", "claude-3-5-sonnet-20241022")
cap("cap_anthropic_37", "ANTHROPIC", "claude-3-7-sonnet-20250219")
cap("cap_anthropic_40", "ANTHROPIC", "claude-opus-4-0")
cap("cap_anthropic_45", "ANTHROPIC", "claude-sonnet-4-5")
cap("cap_anthropic_46", "ANTHROPIC", "claude-opus-4-6")
cap("cap_anthropic_46_dot", "ANTHROPIC", "claude-sonnet-4.6")
cap("cap_anthropic_4_2025", "ANTHROPIC", "claude-opus-4-2025-01-01")
cap("cap_anthropic_undocumented", "ANTHROPIC", "claude-opus-5")
cap("cap_anthropic_non_claude", "ANTHROPIC", "my-claude")
cap("cap_anthropic_trim_case", "ANTHROPIC", "  CLAUDE-OPUS-4-6  ")
cap("cap_openai_default", "OPENAI", "gpt-9")
cap("cap_openai_astra", "OPENAI", "gpt-6-astra")
cap("cap_openai_sol", "OPENAI", "gpt-6-sol")
cap("cap_openai_luna_trim", "OPENAI", " GPT-6-Luna ")
cap("cap_ollama_gpt_oss", "OLLAMA", "gpt-oss:20b")
cap("cap_ollama_library", "OLLAMA", "library/gpt-oss:latest")
cap("cap_ollama_llama", "OLLAMA", "llama3.2")
cap("cap_qwen38_max", "QWEN", "qwen3.8-max")
cap("cap_qwen38_max_dated", "QWEN", "qwen3.8-max-2026-01-01")
cap("cap_qwen38_flash", "QWEN", "qwen3.8-flash")
cap("cap_qwen38_27b", "QWEN", "qwen3.8-27b")
cap("cap_qwen38_24t", "QWEN", "qwen3.8-2.4t-a95b")
cap("cap_glm53", "QWEN", "glm-5.3")
cap("cap_glm53_zhipu", "QWEN", "zhipu/glm-5.3-air")
cap("cap_deepseek_v4", "QWEN", "deepseek-v4-pro")
cap("cap_kimi_k3", "QWEN", "kimi-k3")
cap("cap_kimi_path", "QWEN", "kimi/kimi-k3-latest")
cap("cap_qwen_thinking_only", "QWEN", "qwen3-next-80b-a3b-thinking")
cap("cap_qwq_plus_prefix", "QWEN", "qwq-plus-2025-01-01")
cap("cap_qwen_hybrid", "QWEN", "qwen-plus")
cap("cap_qwen_third_party", "QWEN", "some-third-party")
cap("cap_groq_gpt_oss", "GROQ", "openai/gpt-oss-120b")
cap("cap_groq_qwen36", "GROQ", "qwen/qwen3.6-27b")
cap("cap_groq_qwen38", "GROQ", "qwen/qwen3.8-27b")
cap("cap_groq_unknown", "GROQ", "llama-3.3-70b")
cap("cap_gemini", "GEMINI", "gemini-2.5-pro")
cap("cap_openrouter", "OPENROUTER", "openai/gpt-5")
cap("cap_deepseek_family", "DEEPSEEK", "deepseek-chat")
cap("cap_compatible", "OPENAI_COMPATIBLE", "custom-model")
cap("cap_local", "LOCAL", "local-model")

# ---- ModelThinkingCapabilities.forProvider ----------------------------------------------------
case("provider_capability_protocol_wins", op="providerCapability",
     providerName="anthropic", customProtocol="google", modelId="gemini-2.5-pro")
case("provider_capability_name_openai", op="providerCapability",
     providerName="openai", modelId="gpt-6-astra")
case("provider_capability_custom_relay", op="providerCapability",
     providerName="My Relay", customProtocol="anthropic", modelId="claude-opus-4-6")

# ---- ModelThinkingCapability.nearestEffort ----------------------------------------------------
nearest("nearest_anthropic_minimal", "ANTHROPIC", "claude-opus-4-6", "minimal")
nearest("nearest_anthropic_medium", "ANTHROPIC", "claude-opus-4-6", "medium")
nearest("nearest_anthropic_xhigh", "ANTHROPIC", "claude-opus-4-6", "xhigh")
nearest("nearest_anthropic_max", "ANTHROPIC", "claude-opus-4-6", "max")
nearest("nearest_anthropic_none", "ANTHROPIC", "claude-opus-4-6", "none")
nearest("nearest_qwen_hybrid", "QWEN", "qwen-plus", "high")
nearest("nearest_qwen38_high", "QWEN", "qwen3.8-max", "high")
nearest("nearest_qwen38_medium", "QWEN", "qwen3.8-max", "medium")
nearest("nearest_deepseek_medium", "DEEPSEEK", "deepseek-chat", "medium")
nearest("nearest_groq_qwen36_high", "GROQ", "qwen/qwen3.6-27b", "high")
nearest("nearest_openrouter_none", "OPENROUTER", "anything", "none")
nearest("nearest_openai_sol_minimal", "OPENAI", "gpt-6-sol", "minimal")
nearest("nearest_ollama_levels_minimal", "OLLAMA", "gpt-oss", "minimal")
nearest("nearest_gemini_max", "GEMINI", "gemini-2.5-pro", "max")
nearest("nearest_unknown_effort", "OPENAI", "gpt-9", "ultra")

# ---- ModelThinkingCapability.clampBudget ------------------------------------------------------
clamp("clamp_qwen_low", "QWEN", "qwen-plus", 1)
clamp("clamp_qwen_high", "QWEN", "qwen-plus", 100000)
clamp("clamp_qwen_zero", "QWEN", "qwen-plus", 0)
clamp("clamp_anthropic_below", "ANTHROPIC", "claude-opus-4-6", 999)
clamp("clamp_anthropic_min", "ANTHROPIC", "claude-opus-4-6", 1024)
clamp("clamp_permissive_max", "OPENAI_COMPATIBLE", "anything", 2147483647)
clamp("clamp_groq_no_ceiling", "GROQ", "llama-3.3-70b", 999999)

# ---- ThinkingResolution.resolve ---------------------------------------------------------------
resolve("resolve_disabled_disableable", "OPENAI", "gpt-9", False, "high")
resolve("resolve_disabled_forced_astra", "OPENAI", "gpt-6-astra", False, "high")
resolve("resolve_disabled_forced_no_effort", "QWEN", "qwq-plus", False, "high")
resolve("resolve_enabled_plain", "OPENAI", "gpt-9", True, "high")
resolve("resolve_enabled_unknown_effort", "OPENAI", "gpt-9", True, "ultra")
resolve("resolve_budget_unsupported", "OPENAI", "gpt-9", True, "low", True, 4096)
resolve("resolve_budget_qwen", "QWEN", "qwen-plus", True, "high", True, 99999)
resolve("resolve_budget_qwen_min", "QWEN", "qwen-plus", True, "high", True, 0)
resolve("resolve_budget_anthropic", "ANTHROPIC", "claude-opus-4-6", True, "high", True, 8000)
resolve("resolve_budget_anthropic_clamped", "ANTHROPIC", "claude-opus-4-6", True, "high", True, 100)
resolve("resolve_budget_openrouter", "OPENROUTER", "anything", True, "high", True, 5000)
resolve("resolve_effort_qwen38", "QWEN", "qwen3.8-max", True, "high")
resolve("resolve_budget_qwen38", "QWEN", "qwen3.8-max", True, "high", True, 50000)
resolve("resolve_ollama_max", "OLLAMA", "gpt-oss", True, "max")
resolve("resolve_forced_off_openrouter", "OPENROUTER", "anything", False, "max")


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
