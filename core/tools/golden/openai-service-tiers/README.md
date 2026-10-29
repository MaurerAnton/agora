# openai-service-tiers golden stand

Parity pin for `model/OpenAiServiceTiers.kt` — the service-tier policy shared by settings
persistence (`normalize` on write) and request building (`requestValue`). The source compiles
**verbatim from the app tree** (zero imports) and the canonical trace is frozen under `golden/`;
the shipped port must reproduce every trace byte for byte
(`core/tests/openai_service_tiers_parity_test.cpp`).

## Why this module exists

`openai_request.cpp` only forwards an already-resolved `service_tier` to the wire. The resolution
that happens one layer up in the Kotlin app — `GenerationRequestBuilder` calls
`OpenAiServiceTiers.requestValue(...)` and every settings write goes through `normalize` — had no
counterpart in `core/`. This stand ports and pins the whole policy:

| Kotlin | C++ under test |
| --- | --- |
| `OpenAiServiceTiers.values` | `tiers::values()` |
| `normalize` | `tiers::normalize` |
| `availableTiers` | `tiers::available_tiers` |
| `mappedTier` | `tiers::mapped_tier` |
| `indexForTier` | `tiers::index_for_tier` |
| `tierForIndex` | `tiers::tier_for_index` |
| `requestValue` | `tiers::request_value` |

## What is pinned

* **normalize** — trim + lowercase, then the legacy aliases (`scale` → `default`,
  `priority` → `fast`) and finally the strict membership check falling back to `auto`. Pinned for
  every value, null/empty/blank, upper case, tab padding, NBSP padding (Kotlin `trim()` removes
  NBSP) and near-miss strings (`flexx`).
* **availableTiers** — a non-official provider always gets all five tiers regardless of the model
  id. An official provider gets the full list for unknown/unreadable ids and the per-model filter
  for known ones: flex-only (`gpt-5.5-pro`), fast-only (`gpt-4o`, `gpt-5.3-codex`), standard-only
  (`gpt-5-pro`, `gpt-5.2-pro`, `o1-pro`, `davinci-002`) and the one ultrafast model
  (`gpt-5.6-sol`), plus case/whitespace id normalization.
* **mappedTier** — supported tiers pass through; `flex` falls back to `default`; `ultrafast`
  falls back to `fast` when the model serves fast (`gpt-5.6-luna`, `gpt-5.4`) and to `default`
  otherwise; `auto`/`default` always survive. The symmetric `fast → ultrafast` branch is ported
  but unreachable with the current tables (the ultrafast model is also a fast model), so no
  trace can distinguish it — that is a property of the tables, not a gap.
* **indexForTier / tierForIndex** — normalize never misses, so the index is always 0..4; the
  index clamps at both ends (`-5`, `99`).
* **requestValue** — the wire value exists only when tiering is enabled **and** the Responses API
  is enabled; otherwise the result is null regardless of the mapped tier. Pinned through all four
  enabled/responses combinations, including a disabled request whose model would otherwise map.

## Documented boundaries

* `lowercase()` is Unicode-aware in Kotlin; the port folds ASCII only, like the other core
  modules. Every tier string and model id in the tables is ASCII, so the two agree on all
  reachable inputs; an exotic fold (e.g. the Kelvin sign inside `fleW`) could differ and is not
  pinned.
* `requestValue` evaluates `mappedTier` before `takeIf` in Kotlin; the port short-circuits instead.
  `mappedTier` is pure, so the observable result is identical.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`core/build/agora_scenarios openai-service-tiers core/tools/golden/openai-service-tiers` rewrites `scenarios/` deterministically (92 cases); rerun
`./run_golden.sh --freeze` afterwards.
