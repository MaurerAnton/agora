# conch-sse golden stand

Parity pin for the native port of `tool/ConchSseParser.kt`. Every scenario under `scenarios/`
runs through the **real Kotlin parser compiled verbatim from the app tree** and the canonical
trace is frozen under `golden/`; the C++ port (`agora::core::conch::parse_conch_sse_lines`, test
`core/tests/conch_sse_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 42 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `ConchSseTraceDump.kt` | The stand driver (feeds the parser raw SSE lines) |
| `agora_extract shell-prefix` | Lifts `shellUtf8Prefix` + its byte budget out of `ShellClient.kt` |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

Conch speaks a strict one-event/one-data SSE protocol. Streaming output is intentionally emitted
before the terminal frame arrives; malformed, unknown, incomplete, duplicate-terminal and
post-terminal input all fail closed.

* **19 distinct failure messages**, each pinned verbatim: an event with no data frame, data with
  no event, input after a terminal frame, an unsupported event, a line that is not SSE, invalid
  JSON, a missing/blank/unsupported field, a typed field given the wrong JSON type, a stream that
  ends without a terminal frame, an authentication/sequence mismatch, and a decryption failure.
* **Streaming output survives a later failure** — deltas already handed to `onOutput` stay in the
  trace, because that is the protocol's contract with the UI.
* **One shared byte budget** (`SHELL_COMMAND_OUTPUT_MAX_BYTES` = 1 MiB) for retained *and*
  emitted output. `ascii_output_is_truncated_at_the_byte_budget` and
  `multibyte_character_is_never_split` pin this: in the second one the first frame consumes all
  but 2 bytes, so the next frame emits exactly `ta`, the three-byte `€` is never split, and the
  truncation warning is recorded exactly once.
* **Encrypted mode** authenticates every frame: `_conch_event` must match the `event:` line and
  `_conch_sequence` must be a non-string number counting up from 0. A missing field raises
  `requires string _conch_event` (validation runs before the mismatch check, exactly as Kotlin's
  left-to-right `||` does).
* **Warnings** are capped at 8192 characters and joined with newlines; the truncation notice is
  prepended rather than appended.

## Scenario format (`scenarios/*.json`)

```json
{
  "name": "...",
  "encrypted": false,
  "lines": ["event: line", "data: {...}"],
  "decrypt_error": "..."
}
```

`decrypt_error`, when present, makes the decrypt callback throw instead of returning.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
encrypted|0/1
emit|<escaped delta>              (one per onOutput call, in order)
output|<escaped final output>     (result only)
exit_code|<n or ->                (result only)
error_message|<escaped or ->      (result only)
timed_out|0/1                     (result only)
warning|<escaped or ->            (result only)
error|<escaped message>           (instead of the result block, when the parser throws)
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`.

## Scope and boundaries

* `ConchSseParser.kt` compiles verbatim. Its two dependencies in `ShellClient.kt` —
  `shellUtf8Prefix` and `SHELL_COMMAND_OUTPUT_MAX_BYTES` — are lifted out verbatim by
  `agora_extract shell-prefix`, because that file as a whole pulls in `ShellCrypto`,
  `ConchNetwork`, `ConchHandshake` and `DebugLog` (transport and Android-only logging).
  The extractor fails loudly if either declaration changes shape.
* **Cancellation is not exercised**: the parser's `currentCoroutineContext().isActive` and
  `ensureActive()` calls only matter when the caller cancels the coroutine, which a synchronous
  driver cannot do. Every other branch, including both post-loop checks, is pinned.
* **Unpaired surrogates** produced by a JSON escape such as `\ud800` render as `?`, matching
  Kotlin's UTF-8 encoder replacement (`lone_surrogate_in_line`). Reproducing the *byte-budget
  accounting* for such input would require modelling UTF-16 length separately from byte length,
  which the scenarios never reach — the parser treats input as well-formed UTF-8.
* The 8192-character warning cap and `String.take` are counted in UTF-16 units in Kotlin and in
  bytes here; scenarios only exercise ASCII, so the two agree.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (its git-ignored scratch, including the extracted
`ShellPrefix.kt`) before each run, so runs are idempotent. As with the other stands, kotlinc ≤ 1.9
cannot read the class file version of newer JDKs, hence the explicit `JAVA_HOME`.
