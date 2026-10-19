# thinking-capability golden stand

Parity pin for the native port of the thinking-capability cluster. Every scenario under
`scenarios/` runs through the **real Kotlin sources compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::thinking_capability`, header `core/include/agora/core/thinking_capability.hpp`,
implementation `core/src/thinking_capability.cpp`, test
`core/tests/thinking_capability_parity_test.cpp`) must reproduce every trace byte for byte.

## What the stand compiles, and why

| File | Role |
| --- | --- |
| `model/ModelThinkingCapability.kt` | `ModelThinkingCapability`, `ResolvedThinking`, `ThinkingResolution` |
| `model/ThinkingControl.kt` | `ThinkingLevels` (vocabulary, normalization, legacy budget) |
| `model/ModelThinkingCapabilityDefaults.kt` | `ThinkingProviderFamily` and the documented per-model table |
| `model/ModelThinkingCapabilities.kt` | `familyFor` / `forModel` / `forProvider` |

All four files have **no imports at all**, so the stand needs no stubs and no source extraction.
`ModelThinkingCapability.kt` calls `kotlin.math.abs` fully qualified, which is stdlib only.

## What is pinned

* **normalization vocabulary** — `min`/`quick`/`fast`/`balanced`/`deep`/`x-high`/`extreme`/
  `maximum` aliases; `none` survives; unknown input falls back to `medium`; blank, `auto`,
  `budget:dynamic` and any `budget:*` migrate to `medium`; Kotlin whitespace is used, so an
  NBSP-only value is blank.
* **index mapping** — `effortForIndex` falls back to `medium` outside `0..5` (including negative
  indices); `indexForEffort` normalizes through the *private* effort normalizer, so `auto` and
  `budget:*` land on `medium = 2`, while `none` is not in the vocabulary and coerces to `0`.
* **family resolution** — a non-blank custom protocol always wins over the display name; the
  protocol spelling `openai` maps to `OPENAI_COMPATIBLE` while the display name `openai` maps to
  `OPENAI`; names and protocols are trimmed and case-folded; a blank protocol falls back to the
  name; unknown names map to `OPENAI_COMPATIBLE`.
* **per-model refinements** — every branch of `forModel`:
  * Anthropic: 3.x ids (no effort selector, no budget, sampling allowed), 3.7–4.5 (budget only,
    floor 1024, sampling allowed), 4.6 (full effort set, sampling allowed), other `claude` ids and
    non-`claude` ids get the permissive entry with only the 1024 floor;
  * OpenAI: `gpt-6-astra` cannot disable thinking and has no `minimal`; `gpt-6-sol`/`gpt-6-luna`
    add `none`; everything else is the family default;
  * Ollama: only the bare `gpt-oss` id (path and tag stripped) gets levels, every other id is
    boolean;
  * Qwen: the `qwen3.8` effort group, `glm-5.3` always-thinking, DeepSeek/Kimi served through
    DashScope (budget ceiling 32768), documented thinking-only snapshots, the hybrid fallback and
    the permissive third-party fallback;
  * Groq: GPT-OSS cannot disable thinking, Qwen 3.6 27B accepts only `default`, Qwen 3.8 27B uses
    the family levels, unknown ids are permissive without a budget.
* **nearest effort** — normalized first; no selector returns null; a value outside the vocabulary
  (`none`) falls to the first accepted level; ties resolve **upward**, so Qwen 3.8 asked for
  `high` gets `xhigh` and DeepSeek asked for `medium` gets `high`.
* **budget clamping** — floors (`1` or Anthropic's `1024`), ceilings (`32768` for Qwen, none
  elsewhere, observed as `Int.MAX_VALUE`) and in-range values.
* **resolution** — disabled thinking returns null effort/budget when the model can disable it;
  a forced-thinking-off model keeps `enabled = true` and reports its lowest accepted effort (or
  null when it has no selector) without inventing a budget; a requested budget is applied only
  when the model supports one; the effort is dropped when effort and budget cannot be sent
  together; unknown effort strings normalize before selection.

## Boundary note

`ThinkingLevels.normalize`/`indexForEffort` call Kotlin `String.lowercase()`, which is
Unicode-aware. Like the other core ports, this implementation folds **ASCII A–Z only**. Every
literal the cluster compares against (family names, protocol names, model-id prefixes) is ASCII,
and the scenarios use only ASCII letters, so the pinned behaviour is exact; `trim()` is
Unicode-aware and uses the JVM whitespace table, which the NBSP scenario pins.

## Scenario format (`scenarios/*.json`)

One operation per file; `op` selects it:

```json
{"name": "family_protocol_wins", "op": "family",
 "providerName": "anthropic", "customProtocol": "google"}
```

| `op` | Fields | Result |
| --- | --- | --- |
| `normalize` | `raw` (nullable) | normalized effort |
| `legacyBudget` | `raw` (nullable) | tokens or `null` |
| `effortForIndex` | `index` | effort |
| `indexForEffort` | `effort` | index |
| `family` | `providerName` (nullable), `customProtocol` (nullable) | family name |
| `familyDefault` | `family` | capability |
| `capability` | `family`, `modelId` | capability |
| `providerCapability` | `providerName` (nullable), `customProtocol` (nullable), `modelId` | capability |
| `nearest` | `family`, `modelId`, `effort` | effort or `null` |
| `clamp` | `family`, `modelId`, `tokens` | clamped tokens |
| `resolve` | `family`, `modelId`, `enabled`, `effort`, `budgetEnabled`, `budgetTokens` | `ResolvedThinking` |

A null field and an absent field are equivalent. Capability and resolution results are printed
with Kotlin's data-class `toString()`, so every declared field, in declaration order, is pinned —
including the effort list order and the `maxBudgetTokens` nullability.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
args|<operation-specific arguments joined by |>
result|<operation result>
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`. A null argument or a null
result prints `-`/`null`, while an empty string is escaped as itself.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent. The
C++ test reads `scenarios/` and `golden/` straight from this directory through
`AGORA_FIXTURE_THINKING_CAPABILITY_DIR`.
