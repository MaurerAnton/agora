# task-schedule golden stand

Parity pin for the native port of `automation/TaskSchedule.kt`. Every scenario under `scenarios/`
runs through the **real Kotlin model compiled verbatim from the app tree** and the canonical trace
is frozen under `golden/`; the C++ port (`agora::core::schedule`, test
`core/tests/task_schedule_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 87 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `TaskScheduleTraceDump.kt` | The stand driver (builds a schedule, calls one operation) |
| `agora_extract task-entity` | Lifts `TaskEntity` out of `data/local/ChatEntities.kt` |
| `PlatformStubs.kt` | The `@Entity` / `@PrimaryKey` Room markers |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `agora_scenarios task-schedule` | Generates the scenarios (deterministic; rerun after editing) |

`TaskSchedule.kt` itself needs **no stubs** — it only imports `java.util.Calendar`. Its
`hasSchedule()` extension is declared on the Room entity `TaskEntity`, which lives in a file full
of other entities that cannot compile host-side, so `agora_extract task-entity` copies that one
declaration out verbatim and only the two Room markers are redirected at stand-local stubs. Room is
persistence, not logic, which is what makes that a platform-type substitution.

## What is pinned

**`toCron`** — a one-shot is `""` because a 5-field cron has no year; a weekly schedule with nothing
ticked degrades to the daily form rather than emitting a malformed field; and the day selection is
**sorted on the way out** even though the model stores it in insertion order, which the traces show
side by side (`WEEKLY,8,45,5,1,3` in, `45 8 * * 1,3,5` out). A daily schedule keeps a stale day
selection and ignores it.

**`parse`** reads a stored cron back, and the rejections are the interesting half. It takes **only a
single literal minute and hour**: the hourly preset, a step, a range or a list of minutes are all
returned as "not expressible" so the caller leaves the expression alone rather than rewriting it
into an approximation. Both day fields restricted is also rejected, because cron ORs them and no
single editor row can say that. A positive `runAt` wins over the cron; a zero one does not.

**A space inside a list breaks the whole expression.** `0 6 * * 1, 3` splits on `\s+` into six
fields and is rejected — the `trim()` inside `cronValueList` is unreachable, because the field split
already happened. `0 6 * * ` likewise trims to four fields.

**Day-of-week is folded and then deduplicated as a `LinkedHashSet`**: `0,7` becomes a single Sunday,
and the resulting order is the cron's, not sorted — `0 6 * * 5,1` yields `[5, 1]`.

**Lenient calendar arithmetic is pinned.** `withOnceAt(2027, 2, 29)` produces the instant for
**1 March 2027** — but stores `dayOfMonth = 29, month = 2` exactly as asked, so validation can
reject a past date instead of the year moving underneath the user. April 31 becomes May 1.

**`withTime`** on a one-shot takes the date from the stored epoch, not from the `dayOfMonth` and
`month` fields, and rebuilds the instant — including across a daylight-saving change, where the
resulting date rolls over.

**`formatOnDate`** pins two things a naive port gets wrong. The date-less types render against a
**fixed leap year (2000)**, which is what lets February 29 survive — but an impossible date still
rolls over, because the Kotlin reads `cal.time` out of a lenient `Calendar`: February 30 renders as
`Mar 1` and April 31 as `May 1`. And `ONCE` renders the **epoch**, ignoring the stored
`dayOfMonth`/`month` fields entirely. Formatting is locale-driven and pinned for `en-US`, `de-DE`
(`März 14`) and `fr-FR` (`mars 14`).

**`hasSchedule`** needs a **positive** epoch: a blank cron with `runAt = 0` is unscheduled, which is
the whole reason the extension exists — a one-shot must never read as unscheduled and get skipped.

## Scenario format (`scenarios/*.json`)

```json
{"name": "...", "op": "switchedTo", "tz": "Europe/Berlin", "locale": "en-US",
 "schedule": {"type": "ONCE", "hour": 14, "minute": 30, "daysOfWeek": [3],
              "dayOfMonth": 10, "month": 6, "onceAtMillis": 1780000000000},
 "newType": "WEEKLY"}
```

`op` is one of `toCron`, `toRunAt`, `parse`, `switchedTo`, `withOnceAt`, `withTime`,
`formatOnDate`, `hasSchedule`. The driver pins the JVM's default time zone and locale from the
scenario and records both in the trace, so the `Calendar` and `SimpleDateFormat` behaviour is
deterministic and pinned against known settings.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
op|<operation>
tz|<resolved zone id>
locale|<language tag>
schedule|<type>,<hour>,<minute>,<days>,<dayOfMonth>,<month>,<onceAtMillis>   or -
args|<escaped arguments joined by |>
out|string|<escaped>  |  out|long|<millis or ->  |  out|bool|0|1  |  out|schedule|<serialized or ->
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\\\` `\\p` `\\s` `\\n` `\\r` `\\t`.

## Scope and boundaries

* **The wall clock cannot be frozen, so `switchedTo` is only partly pinned.** The Kotlin reads
  `Calendar.getInstance()` to seed missing fields and `System.currentTimeMillis()` to decide whether
  a one-shot is still in the future. The JDK offers no supported way to override either, and
  `libfaketime` is not available here, so a verbatim stand simply cannot produce the same trace
  twice. Golden-pinned are the clock-free paths: `newType == type`, and every switch from a MONTHLY,
  YEARLY or ONCE source into DAILY or WEEKLY **with a non-empty day selection**. Not pinned are:
  seeding days-of-week, day-of-month or month from today (any empty day selection, and any DAILY or
  WEEKLY source), `switchedTo(ONCE)` in general, and `withTime` on a one-shot whose epoch is 0.
  The port implements all of them; it takes the zone and the current instant as an explicit
  `schedule::Context` rather than reading globals, which is the better interface anyway.
* **Month names live outside the core.** The Kotlin hands the pattern to `SimpleDateFormat` with
  `Locale.getDefault()`; the port asks the caller for the abbreviation and returns it, so locale
  data stays a presentation concern. The parity test carries the tables for the three pinned
  locales.
* `daysOfWeek` is kept in insertion order rather than sorted, because `parse()` builds it with
  `toSet()` and `toCron()` sorts explicitly — a difference the traces make visible.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
core/build/agora_scenarios task-schedule core/tools/golden/task-schedule              # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (its git-ignored scratch, including the extracted
`TaskEntity.kt`) before each run, so runs are idempotent.