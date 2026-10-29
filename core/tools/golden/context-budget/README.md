# context-budget golden stand

Parity pin for `model/ContextBudget.kt` — the shared bounds and legacy-setting migration for the
provider-visible conversation token budget, plus the compact occupancy label used by the settings
and chat bars. The source compiles **verbatim from the app tree** (zero imports) and the canonical
trace is frozen under `golden/`; the shipped port must reproduce every trace byte for byte
(`core/tests/context_budget_parity_test.cpp`).

## Why this module exists

`ContextBudget` is called from `GenerationRequestBuilder` (prompt budgeting), `SettingsManager` /
`SettingsRepository` (persisted settings) and four UI surfaces (bottom bar, top bar, context
composition bar, conversation settings). None of it had a counterpart in `core/`. The port lives
in `agora::core::context_budget`:

| Kotlin | C++ under test |
| --- | --- |
| `DEFAULT_TOKENS`, `MIN_TOKENS`, `MAX_TOKENS`, `PRESETS` | `context_budget::kDefaultTokens`, `kMinTokens`, `kMaxTokens`, `presets()` |
| `normalize` | `context_budget::normalize` |
| `compactLabel` / `compactUnitLabel` | `context_budget::compact_label` |

## What is pinned

* **legacy migration** — `null`, `0` and negative values return `DEFAULT_TOKENS`; values `1..100`
  are logical-message windows multiplied by 1024 (`4 → 4096`, `50 → 51200`, `64 → 65536`,
  `100 → 102400`); `101` is already a token value and clamps up to `4096`. The edge between the
  migrated and non-migrated branches is pinned on both sides.
* **clamp** — everything lands in `[4096, 1048576]`: `1 → 4096`, `4095 → 4096`, `1000 → 4096`
  (non-legacy but below the floor), `2000000 → 1048576`, `Int.MAX_VALUE → 1048576`.
* **compact labels** — negative occupancy is `0`; values up to `1000` stay decimal (`1000` itself
  is `"1000"`, not `"1K"`); `1001..1048575` use the `1024` unit with one decimal, half-up
  (`1001 → 1K`, `1536 → 1.5K`, `4097 → 4K`, `65536 → 64K`, `1000000 → 976.6K`,
  `1048575 → 1024K`); `>= 1048576` uses the `1048576` unit (`1048576 → 1M`, `1572864 → 1.5M`,
  `5242880 → 5M`, `Int.MAX_VALUE → 2048M`). The rounding uses Long math in Kotlin; the port does
  the same, which is why `Int.MAX_VALUE` does not overflow.
* **no migration in labels** — `compactLabel(64)` is `"64"`, not `"64K"`: the comment in the
  Kotlin source is a trap for readers and is pinned by the low-value cases.

## Documented boundaries

* Kotlin's `Int` arithmetic is modelled with `long long` internally and converted back, which is
  exact for every `Int` input (the only multiplication is `value * 1024` for `value <= 100`).

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`core/build/agora_scenarios context-budget core/tools/golden/context-budget` rewrites `scenarios/` deterministically (45 cases); rerun
`./run_golden.sh --freeze` afterwards.
