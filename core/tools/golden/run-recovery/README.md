# run-recovery golden stand

Pins `model/RunRecoveryPolicy.kt`: process-death recovery of model message status, incomplete tool
segments, and persisted tool segments in raw JSON.

`RunRecoveryPolicy.kt` compiles **verbatim** from the app tree. Its model types live in
`model/ChatMessage.kt`, which cannot compile host-side because the file also pulls in Compose, Room
and the whole UI model. `agora_extract run-recovery-models` copies the six needed declarations
(`ToolImageAttachment`, `StreamingTextDelta`, `MessageSegment`, `ToolExecutionStates`, `Participant`,
`MessageStatus`) out **byte for byte**; the only stand-local surface is the `@Immutable` Compose
marker in `stubs/`, a compile-time annotation with no runtime behavior. The extracted file is
generated on every run, so the stand cannot silently drift onto a stale copy.

## What is frozen

54 traces cover:

- `recoverMessageStatus`: the full `Participant` × `MessageStatus` matrix (21 pairs).
- `stopIncompleteTools`: empty input, every terminal state (`succeeded`, `empty`, `failed`,
  `stopped`, `background_running`), `calling`, `running`, missing state, unknown/case variants,
  non-tool segments, mixed order, and one segment with every constructor field populated. The trace
  uses the real data-class `toString()` before and after, so untouched fields (including both
  `@Transient` ones) are part of the contract.
- `stopIncompleteToolsJson`: unchanged roots returned byte for byte (whitespace included),
  non-object elements, null/number/bool/array/object states, terminal and non-terminal tool
  segments, missing state appended at the end, existing state replaced in place, compact
  re-encoding, nested key order, duplicate keys (`type`, `toolState`), Unicode and escapes,
  non-array roots and malformed JSON returning `null`.

## Run

```bash
export JAVA_HOME=...
export PATH="$JAVA_HOME/bin:$PATH"
export KOTLINC=/path/to/kotlinc
export KOTLINX_JARS=/path/to/kotlinx-serialization-core-jvm.jar:/path/to/kotlinx-serialization-json-jvm.jar
./run_golden.sh            # verify scenarios against golden/
./run_golden.sh --freeze   # regenerate traces after changing scenarios or the Kotlin source
```

The C++ side is checked by `core/tests/run_recovery_parity_test.cpp` (test
`run_recovery_golden_parity`), which iterates the same scenarios and compares the complete trace
byte for byte.
