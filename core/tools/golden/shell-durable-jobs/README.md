# shell-durable-jobs

Golden stand for `ShellDurableJobExecutor.kt` — the Conch durable-job orchestration inside
`ShellToolProvider`: the terminal job-state rules, the rolling output snapshot/cursor
arithmetic, both wait/poll loops, and the terminal acknowledgements derived from committed
tool results.

## What compiles verbatim

| Source | Notes |
| --- | --- |
| `app/src/main/java/com/newoether/agora/tool/ShellDurableJobExecutor.kt` | **verbatim** — no edits at all |
| `app/src/main/java/com/newoether/agora/tool/ShellToolJson.kt` | **verbatim** — `parseToolArgs`/`arg`/`jsonError` |

## Platform doubles (`platform/`)

1. **`GenerationContext` / `ShellDeviceConfig` / `ToolCallData`** — stand-local leaf data
   classes carrying exactly the fields the executor reads (`shellDevices`, `toolTimeoutMs`,
   `toolName`/`arguments`/`result`, device `name`/`type`). The real classes live in
   `GenerationContracts.kt`, `SettingsContracts.kt` and `ChatMessage.kt` and pull in the whole
   app configuration graph.
2. **`ConchBackend`** — scripted double for the real `ConchBackend` in `ShellToolBackends.kt`
   (which owns the OkHttp streaming transport). It exposes the same surface the executor uses
   (`device`, `startJob`, `getJob`, `listJobs`, `stopJob`, `close`), answers from the scenario
   script (`ok` payload / `throw` message / `cancel`-step), records every backend call as an
   event so the trace can pin the interaction, and a `cancel` step cancels the calling
   coroutine exactly like a generation Stop.

Coroutines (`kotlinx-coroutines-core-jvm`) and `kotlinx-serialization-json` are the only
runtime jars; the driver runs each suspending call in an independent coroutine so a scripted
cancellation is observable as either a normal return (the loop's own timeout branch won first)
or a `CancellationException`.

## Scenario ops

| Op | Pins |
| --- | --- |
| `resolve_conch_device` | case-insensitive name match, blank-server single lookup, ssh exclusion |
| `conch_not_found` | all four message branches (none/unknown/multiple/single) + `Untitled` names |
| `is_terminal_payload` | lifecycle-state table, state-wins-over-error, `error`-only terminality, non-primitive/missing/`null`/number states, duplicate-key (last wins), unparseable/blank/array inputs |
| `max_wait_ms` | `toolTimeoutMs - 5000` floor 1000, `Long.toInt()` bit truncation |
| `output_snapshot` | string-output gate, non-negative integral `output_bytes`, strict `truncated` with the byte-count fallback, Unicode/astral byte sizes, every null branch |
| `output_cursor` | global byte cursor: initial emit, append, evicted-window gaps, duplicates, out-of-order/backwards payloads, missing fields, REPLACE-decode of a split tail |
| `wait_for_job_polling` | the wait loop on a **virtual clock**: terminal/wait-expiry/poll-failed(5) JSON, snapshot publication and dedupe, the clamp note, backoff timings (final `clock`), cancellation during poll / after delay / before timeout (which branch wins), cancellation propagation, script exhaustion |
| `wait_for_job` | public entry on the **real clock**: argument validation order, device resolution, `coerceIn(1000, ceiling)` + clamp reporting, `finally close`; `waited_ms` is masked on both sides |
| `durable_foreground` | start→poll orchestration: verbatim passthrough of error/unparseable start payloads, missing `job_id`, poll-failed JSON, cursor deltas + `[Conch output gap: …]` notices, wait expiry, cancellation that stops the remote job (event order), cancellation that loses to the expiry branch; `waited_ms` masked |
| `list_jobs` / `get_jobs` | passthrough vs jsonError, close-on-all-paths, resolution before backend construction, `job_id` null-primitive (`"null"` content) |
| `terminal_acks` | tool whitelist, terminal-state gate (nested `result` wins over envelope), job-id fallback chain, server fallback (envelope → nested → arguments → `""`), blank-`job_id` fallthrough, `distinctBy(lower(server), jobId)`, non-object/non-JSON results |

## Layout

```
shell-durable-jobs/
├── ShellDurableJobTraceDump.kt   trace driver (Kotlin reference execution)
├── platform/                     documented leaf doubles (see above)
├── gen_scenarios.py              deterministic scenario corpus (146 scenarios)
├── scenarios/                    generated JSON inputs
├── golden/                       frozen traces (byte-exact reference output)
├── run_golden.sh                 compile + run + verify (or --freeze)
└── README.md
```

## Running

```sh
KOTLINC=/path/to/kotlinc/bin/kotlinc \
KOTLINX_JARS=/path/kotlinx-serialization-core-jvm.jar:/path/kotlinx-serialization-json-jvm.jar \
./run_golden.sh            # verify against golden/
./run_golden.sh --freeze   # regenerate golden/ from the Kotlin reference
```

The kotlinx-coroutines jar is located next to the serialization jars (override with
`KOTLINX_COROUTINES_JAR`). The C++ side is replayed by
`core/tests/shell_durable_jobs_parity_test.cpp`, which drives the same scenario files through
`core/src/shell_durable_jobs.cpp` with the same scripted backend and compares
`{op, scenario, outcome}` traces byte for byte.

## C++ port boundary

`core/src/shell_durable_jobs.cpp` owns all of the logic above. The transport is the
`DurableJobBackend` interface (the real client keeps OkHttp in Kotlin), and the coroutine
mechanics reduce to three hooks — `now`/`delay`/`is_active` — so both loops run unchanged on a
virtual clock in tests and on the real clock in the app. `System.currentTimeMillis` reads are
`hooks.now()`, `kotlinx delay` is `hooks.delay()`, `isActive` is `hooks.is_active()`, and a
cancellation is a `JobCancelled` (from the poll) or a cleared `is_active` (checked exactly
where Kotlin checks it: loop top, and before each delay in the durable loop).

One documented mechanical substitution: a Kotlin exception's `e.message ?: <default>` cannot be
expressed for a C++ exception (what() is never null), so the default strings are unreachable
from the scripted surface — every scripted failure carries its message explicitly.
