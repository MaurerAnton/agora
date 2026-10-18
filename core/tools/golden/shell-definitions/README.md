# shell-definitions golden stand

Parity pin for the native port of the shell tool definition builder (`ShellToolDefinitions.kt`).
Every scenario under `scenarios/` runs through the **real Kotlin source compiled verbatim** and
the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::shell_tools::build_shell_tool_definitions`, test
`core/tests/shell_definitions_parity_test.cpp`) must reproduce every trace byte for byte.

## How the stand works

`ShellToolDefinitions.kt` reads four things from the viewmodel and nothing else, so the stand
compiles the file **unchanged** against two stand-only leaf stubs and one real helper:

* `stubs/…/viewmodel/GenerationContext.kt` — `shellEnabled`, `shellDevices`, `sandboxEnabled`,
  `toolTimeoutMs` (the builder touches no other context field);
* `stubs/…/data/ShellDeviceConfig.kt` — the `name`/`type` pair (`"conch" | "ssh"`);
* `extract_wait_job.py` — copies `ShellDurableJobExecutor.maxWaitMs` and its two constants
  **verbatim** (the executor itself is a 500-line transport-coupled object whose only relevant
  piece is the ceiling embedded in the `wait_for_job` description).

The driver calls `ShellToolDefinitions.build(ctx)` directly — **no transcription**.

## Scenario format and trace

`{name, mode: "definitions", shellEnabled, sandboxEnabled, toolTimeoutMs, devices: [{name, type}]}`

```
name|<scenario name>
mode|definitions
count|<tool count>
tool|<the exact serialized ToolDefinition JSON>      (one line per tool, in build order)
```

## Pinned semantics

* Gating: a disabled shell, or no devices and no sandbox, yields the empty list; the file tools
  and `list_shells`/`execute_shell_command` always come first, the durable-job tools and
  `view_image` only when a non-`ssh` (Conch) device exists (7 vs 12 tools pinned).
* Device names are **quoted** in the model-facing descriptions, prefixed by `Local Sandbox` when
  the sandbox is on; a single server switches the wording to "defaults to the only available
  server" and drops `server` from the required lists.
* A blank Conch device name is advertised as `Untitled` (while the quoted raw name still shows in
  the file-tool descriptions) — pinned.
* `maxWaitMs(ctx) = (toolTimeoutMs - 5000).coerceAtLeast(1000)` is embedded in the `wait_for_job`
  description; the corpus pins the default, the margin boundary (5000 → 1000), just above it
  (5001 → 2000), a huge value and a negative one.
* Property order follows Kotlin's `mapOf` (insertion order, **not** sorted), and every description
  is byte-pinned through the serialized JSON.

## Scope

Only the schema builder. The tool executors themselves (ShellDurableJobExecutor, the Conch SSE
parser, the per-tool result formatting) stay in Kotlin.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
`maxWaitMs` or its constants move.