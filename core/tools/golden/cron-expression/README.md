# cron-expression golden stand

Parity pin for the native port of `automation/CronExpression.kt`. Every scenario under
`scenarios/` runs through the **real Kotlin parser compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port (`agora::core::cron::CronExpression`, test
`core/tests/cron_expression_parity_test.cpp`) must reproduce every trace byte for byte.

**This stand needs no stubs at all.** `CronExpression.kt` depends on nothing but
`java.util.Calendar` and `java.util.TimeZone`, so the whole file compiles host-side unmodified —
the cleanest harness in this repository. The driver needs the kotlinx-serialization runtime only
to read a scenario file; there is no serialization compiler plugin in this build.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 110 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `CronTraceDump.kt` | The stand driver (parses, dumps the field sets, resolves `next`) |
| `run_golden.sh` | Compiles and regenerates or verifies the traces |
| `agora_scenarios cron-expression` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

### Validity alone would hide the grammar

Each parse trace carries `valid`, the **five expanded value sets** and the two restriction flags,
not just whether the expression is well formed. The sets are private in Kotlin, so the driver reads
them reflectively; that is test-only introspection and does not touch the source under test.

* **Steps reach the field max.** `5/10` in the minute field is 5,15,…,55 — a bare value with a step
  runs *up to the field max*, not just to itself. `0-30/10` stops at 30, and `*/60` over a
  0–59 field yields only 0.
* **Day-of-week accepts 7 as Sunday** and folds it onto 0, so `0,7` collapses to a single value and
  `7/2` yields just Sunday. `0-7/2` never reaches 7, because the range ends at 7 and the step
  skips it — it yields 0,2,4,6.
* **`toIntOrNull` accepts a leading plus**, so `+5/10` is a valid minute field. It does *not*
  accept surrounding spaces, which is why a step written as `"/ 5"` never even reaches step
  parsing.
* **Rejected forms are pinned individually**, not lumped together: empty and whitespace-only
  expressions, four or six fields, each field out of its range, step 0, negative step, non-numeric
  step, two slashes, an empty list element, a three-part range, a range missing either bound, and
  a range whose start is above its end.
* **Whitespace is `[ \t\n\x0B\f\r]`, not Unicode whitespace.** A `U+00A0` between two numbers does
  not split, so `1<U+00A0>2 3 4 5` has four fields and is rejected — its own scenario.

### `next()` is where java.util.Calendar decides the answer

* **Seconds are truncated *before* the minute is added, which moves backwards.** From 12:29:59.999
  a `30 12 * * *` schedule still resolves to 12:30:00 the same day, because zeroing the seconds
  lands on 12:29:00 and only then does the walk advance. From 12:30:00.001 it is already past, so
  the answer is the next day.
* **The Vixie rule.** With both day fields restricted a tick matches if **either** does, so
  `0 0 13 * 5` fires on every Friday and also on the 13th. Restricted-dom-only and
  restricted-dow-only both use AND.
* **A spring-forward hour is never reached at all.** `30 2 * * *` in `Europe/Berlin` skips
  2026-03-29 entirely and returns 2026-03-30 02:30, because the hour walk from 01:00 CET lands on
  03:00 CEST and wall time 02:30 never occurs. Pinned for Berlin, New York and Sydney.
* **A fall-back hour resolves to standard time.** `30 2 * * *` in Berlin returns the **second**
  02:30 on 2026-10-25, the CET one. Pinned for Berlin and New York.
* **Minute stepping and hour stepping disagree across a fall-back, and both are pinned.** Walking
  minute by minute from 02:59 CEST in Berlin returns 03:00 CET, because 02:59 + 1 minute of wall
  time is 03:00 and the repeated 02:00–02:59 hour is skipped entirely. Walking hour by hour to
  reach the same `30 2 * * *` match instead lands on 02:30 CET.
* **Fixed-offset zones** are pinned for `GMT+05:30` (JVM spelling: **east**, unlike the POSIX sign
  convention), `Etc/GMT+8` (POSIX spelling: **west**), `Etc/GMT-12`, `Asia/Kolkata` and
  `Asia/Colombo`.
* **An unknown zone id silently becomes GMT**, which `TimeZone.getTimeZone` does rather than
  failing; the trace records the resolved id, so the port has to make the same substitution.
* **Leap day** is pinned both from a date that resolves four years out and from one just past a
  match, which has to wait for the next one.

## Scenario format (`scenarios/*.json`)

```json
{"name": "...", "expr": "0 0 * * 0", "next": true,
 "after_millis": 1773489660000, "zone": "Europe/Berlin"}
```

`next: true` adds the resolution block; without it the trace is parse-only.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
expr|<expression>
valid|0|1
minutes|<comma-separated set>
hours|<comma-separated set>
daysOfMonth|<comma-separated set>
months|<comma-separated set>
daysOfWeek|<comma-separated set>
domRestricted|0|1
dowRestricted|0|1
zone|<resolved zone id>     (resolution cases only)
next|<millis or ->          (resolution cases only)
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\\\` `\\p` `\\s` `\\n` `\\r` `\\t`.

**`daysOfWeek` is listed in insertion order, not sorted order** — `1,4,0` for `1-7/3`. Kotlin builds
that field with `map{}.toSet()`, which is a `LinkedHashSet`, while the other four come out of a
`sortedSetOf`. It makes no difference to matching, but it does change the serialized shape, so the
port keeps a vector for that field alone.

## Scope and boundaries

* `CronExpression.kt` compiles **verbatim**, with no extracted dependencies and no platform stubs.
* The C++ side resolves zones from the **system tz database** (`/usr/share/zoneinfo`, TZif v2),
  while the JVM uses its own bundled copy. The resolution scenarios deliberately stay inside dates
  where every published tzdata agrees on the US, EU and Australian rules — all of them unchanged
  since 2007 — so the two cannot legitimately disagree. Pre-2007 transitions and zones whose rules
  have changed are **not** pinned.
* **The 8-year search horizon is unreachable**, so `nullopt` from `next()` is not pinned: every
  well-formed expression resolves inside it. A Feb-29-only schedule, the case the horizon exists
  for, still resolves four years out.
* `TimeZone.getDefault()` — the default argument of `next` — is not exercised; every resolution
  scenario names its zone explicitly.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios cron-expression core/tools/golden/cron-expression                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (its git-ignored scratch) before each run, so runs are
idempotent.