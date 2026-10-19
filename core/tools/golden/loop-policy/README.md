# loop-policy golden stand

Parity pin for the native port of `automation/LoopPolicy.kt`. Every scenario under `scenarios/`
runs through the **real Kotlin object compiled verbatim from the app tree** and the canonical trace
is frozen under `golden/`; the C++ port (`agora::core::loop_policy`, test
`core/tests/loop_policy_parity_test.cpp`) must reproduce every trace byte for byte.

`LoopPolicy.kt` has **no imports at all** — it is eight constants plus five pure functions — so this
stand needs no stubs and no source extraction. The whole file compiles as written.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 47 frozen cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `LoopPolicyTraceDump.kt` | The stand driver |
| `run_golden.sh` | Compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

Every trace begins with the **constants line**, because the constants are the contract the scheduler
and the model-facing tools share. Widening the interval range or changing the default prompt
silently would otherwise pass every functional scenario.

**`validate`** checks the interval **before** the cycle count, so an input that breaks both reports
the interval (`validate_interval_error_wins`). The accepted range is inclusive at both ends
(`10_000` and `604_800_000` ms, `1` and `100` cycles), and the rejected side is pinned at the exact
neighbour (`9_999`, `604_800_001`, `0`, `101`) plus the `Long`/`Int` extremes.

**`normalizePrompt`** uses Kotlin's `trim()`, i.e. `Char.isWhitespace`, which is not ASCII
whitespace:

* a **no-break space** (U+00A0) is whitespace — a prompt of only NBSP normalizes to `null`, and an
  NBSP-padded prompt is trimmed;
* a **zero-width space** (U+200B) is *not* whitespace — it survives as content. A port that trims
  "everything space-like" gets this wrong, and the trace says so.

Interior whitespace is never collapsed: `" a  b "` becomes `"a  b"` and an interior newline stays.

**`promptForExecution`** falls back to `"Continue."` for `null`, blank and NBSP-only prompts, and
passes a trimmed prompt through otherwise.

**`nextFireAt`** saturates at `Long.MAX_VALUE` on overflow, and the trace pins the guard itself:

* `now = MAX - 60_000` returns exactly `MAX`;
* one millisecond later clamps to `MAX`;
* **a negative interval wraps the guard's own subtraction.** `Long.MAX_VALUE - (-10)` is a large
  *negative* number in two's complement, so almost every `now` satisfies `now > threshold` and the
  result is `MAX`. At `now = Long.MIN` the comparison fails instead, and the *sum* wraps to
  `9223372036854775798` (`MAX - 9`). Both are pinned.

That last pair is why the C++ port does its arithmetic through `std::uint64_t`: Kotlin's `Long`
wraps, C++'s signed overflow is undefined, and the behaviour being ported is the wrap.

**`nextRevision`** returns `0` at `Long.MAX_VALUE` and `current + 1` otherwise.

## Scenario format (`scenarios/*.json`)

```json
{"name": "fire_negative_interval_wraps_threshold", "op": "nextFireAt",
 "now": 0, "intervalMs": -10}
```

`op` is one of `validate` (`intervalMs`, `maxCycles`), `normalizePrompt` (`prompt`),
`promptForExecution` (`prompt`), `nextFireAt` (`now`, `intervalMs`), `nextRevision` (`current`).
A `prompt` that is absent or `null` is distinct from `""`.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
constants|<minIntervalSeconds>|<maxIntervalSeconds>|<minIntervalMs>|<maxIntervalMs>|
          <defaultMaxCycles>|<minMaxCycles>|<maxMaxCycles>|<defaultPrompt>
args|<op-specific arguments joined by |>
out|string?|<escaped message>|-      # validate, normalizePrompt
out|string|<escaped>                 # promptForExecution
out|long|<value>                     # nextFireAt, nextRevision
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
