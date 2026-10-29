# shell-command-validation golden stand

Parity pin for the pre-flight size validation that `tool/ShellToolProvider.kt` runs before a
shell request reaches any backend. Every scenario under `scenarios/` runs through the
**real Kotlin function extracted verbatim from the app tree** and the canonical trace is frozen
under `golden/`; the C++ port (`agora::core::shell_tools::shell_command_validation_error`,
test `core/tests/shell_validation_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 10 frozen scenarios |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ShellValidationTraceDump.kt` | The stand driver (reads command/workdir, prints result) |
| `agora_extract shell-validation` | Lifts `shellCommandValidationError` + its two constants out of the transport-coupled sources |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |

## What is pinned

`shellCommandValidationError(command, workdir)` is a `when` over two UTF-8 byte-count limits:

* **02/03** — the command limit is `SHELL_COMMAND_MAX_BYTES = 64 * 1024` and is an **upper
  bound**: exactly 65536 bytes pass, 65537 fails with `command exceeds 64KB limit`.
* **04/05** — the workdir limit is `SHELL_WORKDIR_MAX_BYTES = 32 * 1024`, likewise
  32768 passes and 32769 fails with `workdir exceeds 32KB limit`.
* **06/08** — the count is **UTF-8 bytes, not chars**: 21846 `€` characters are 65538 bytes
  (command fails) and 10923 `€` characters are 32769 bytes (workdir fails), while the same
  scenarios with ASCII would pass.
* **10** — when both are over, the **command is reported first** (the `when` order).
* **01/07/09** — small, newline-containing and empty input all pass (`<null>`).

## Scenario format (`scenarios/*.json`)

```json
{ "op": "shellCommandValidationError", "command": "ls -la", "workdir": "/tmp" }
```

## Trace format (`golden/*.trace`)

The error message itself (`command exceeds 64KB limit` / `workdir exceeds 32KB limit`), or
`<null>` when the request passes — one line per scenario, `\n` terminated.

## Scope and boundaries

* `agora_extract shell-validation` copies `shellCommandValidationError` out of `ShellToolProvider.kt`
  (763 lines of transport-coupled provider code) and the two limits out of `ShellClient.kt`,
  changing only two things, both recorded in the generated header: the function's visibility
  (`private` → `internal`, so the driver can call it) and the file/package of the constants
  (they are generated next to the function in `com.newoether.agora.tool` instead of
  `com.newoether.agora.util`). The declarations themselves are byte-copied, and the extractor
  fails loudly if either declaration changes shape.
* **Zero platform substitutions**: `Charsets.UTF_8` resolves to the Kotlin built-in, so nothing
  needs shimming and the stand has no `platform/` directory.
* The C++ side counts `std::string_view::size()` (UTF-8 bytes of the decoded text), which is
  exactly `String.toByteArray(Charsets.UTF_8).size` for well-formed input.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (git-ignored scratch, including the extracted
`ShellValidation.kt`) before each run, so runs are idempotent.
