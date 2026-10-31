# Anthropic stream golden stand

Parity pin for the native port of `AnthropicStreamEventRouter`. Every transcript under
`scenarios/` is replayed through the **real Kotlin router** (compiled straight from the app
sources; the `@Serializable` stream-event models are extracted verbatim from
`AnthropicProvider.kt`, which cannot be compiled standalone) and the canonical trace is frozen
under `golden/`. The C++ port (`agora::core::anthropic_stream`, test
`core/tests/anthropic_stream_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.txt` | One transcript per file, 185 frozen edge cases |
| `golden/*.trace` | Frozen canonical output of the Kotlin reference, one per transcript |
| `AnthropicStreamTraceDump.kt` | The reference driver: wires the Kotlin router to the trace format |
| ``agora_extract anthropic-models`` | Copies the stream-event model declarations out of `AnthropicProvider.kt` verbatim |
| `run_golden.sh` | Compiles the reference and regenerates or verifies the traces |
| `agora_scenarios anthropic-stream <stand>` | Generates the transcripts (deterministic; rerun after editing) |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |

## Transcript format

One op per line; blank lines and `#` comments are ignored.

| Op | Meaning |
| --- | --- |
| `L <line>` | Feed one raw SSE line through the provider's read loop (ignored once the loop stopped) |
| `C <raw>` | Call `captureParseError(raw, "driver")` directly, then stop the loop (the decode-catch path is the only caller in the provider) |
| `I` | Call `reportIncompleteBlocks()` directly |
| `E` | Stop reading (end of stream) |

The `L` loop is the semantic half of the provider's read loop: lines starting with `data: ` are
trimmed, decoded with the provider's exact `Json { ignoreUnknownKeys = true; encodeDefaults =
true; explicitNulls = false }`, routed, and the real stopping conditions (`streamError`,
`reportedError`, `messageStopReceived`) are evaluated after every line. After the loop (and after
every `I`) the driver also runs the outer `reportIncompleteBlocks()` call the provider always
performs.

## Trace format

One line per emitted event, in emission order, followed by two summary lines. Escaping is the
citations-stand set (`\\`, `\p` for `|`, `\s` for `;`, `\q` for `"`, `\n`, `\r`, `\t`). Naive
UUIDs are collapsed to `<uuid>` (dashed and compact forms) because they are random in the
reference.

| Line | Meaning |
| --- | --- |
| `text\|<esc>` | `TextChunk` |
| `thought\|<esc>\|title=<esc\|->\|signature=<esc\|->` | `ThoughtChunk` |
| `toolCallUpdate\|streamKey=<norm>\|id=<norm\|->\|name=<esc>\|args=<esc>` | `ToolCallUpdate` |
| `toolCall\|id=<norm>\|name=<esc>\|args=<esc>\|streamKey=<norm>` | `ToolCallRequest` |
| `usage\|total=N\|input=…\|cached=…\|cacheWrite=…\|uncached=…\|output=…\|reasoning=…` | `UsageUpdate` (dash for null) |
| `citation\|v=…;sourceId=…;…;anchors=[{start,end,"text"}]` | `CitationUpdate` (citations-stand field rendering) |
| `error\|api\|code=<esc\|->\|type=<esc\|->\|message=<esc>` | `Error` event carrying a provider error |
| `error\|sseParse\|raw=<esc>` | `Error` event carrying a captured parse/truncation failure |

The summary lines expose the router's terminal state:

```
term|messageStop=0/1|stop=<esc|->|toolUseStarts=N|sawTerminal=0/1|inFlight=0/1|reported=0/1|produced=0/1|driverReported=0/1
streamError|<error line as above|none>
```

`produced` is the provider's `carriesModelOutput()` accumulator and `driverReported` its "some
emitted event was an `Error`" tracker, both inputs to `StreamTermination`.

## Scope and boundaries

* The transport half of the provider is out of scope: socket reads, read timeouts, read errors,
  cancellation, HTTP statuses and the retry loop. This stand pins everything that decides what a
  decoded event MEANS and what the stream has proven about its own end.
* The kotlinx decode-failure *cause texts* are not pinned (kotlinx `localizedMessage` strings are
  implementation detail); the traces pin acceptance/rejection, the stopping effect, and the
  `rawLine.take(512)` cut (UTF-16 units; a split surrogate pair becomes `?`, like the JVM UTF-8
  encoder renders it). The C++ side carries its own descriptive cause.
* `DebugLog` output is a side channel and is not pinned.
* `String.lowercase()` on `stop_reason` is ASCII-only, consistent with the rest of the core port.
* Random UUIDs (`call_stream_<uuid>`, the malformed stand-in) are collapsed in traces; the C++
  test injects deterministic uuids.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
../../../build/agora_scenarios anthropic-stream .                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; ``agora_extract anthropic-models`` fails
loudly if the stream-event model declarations move or change shape.
