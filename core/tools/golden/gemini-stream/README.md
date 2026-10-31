# gemini-stream golden stand

Parity pin for the native port of the **Gemini stream read loop** — the 200-branch half of
`GeminiProvider.generateResponse` that turns raw SSE lines into stream events. Every transcript
under `scenarios/` is replayed through the **real Kotlin sources** and the canonical trace is
frozen under `golden/`; the C++ port (`agora::core::gemini_stream`, test
`core/tests/gemini_stream_parity_test.cpp`) must reproduce every trace byte for byte.

## What is pinned

The loop semantics, in provider order:

1. `data: ` prefix trim; the `[DONE]` marker sets the terminal flag and stops the loop.
2. Response decode (`ApiStreamResponse`, `ignoreUnknownKeys`, the `@JsonNames` alternates
   `executable_code` / `code_execution_result` / `grounding_metadata`).
3. In-band errors: `response.error` becomes `GenerationError.Api` (blank messages fall back to
   "`<providerName>` reported an error in the response stream"), and an `outcome` containing
   "failed to generate" (case-insensitive) becomes `Api(null, "failed_to_generate", outcome)`.
4. `finishReason` through `normalizeGeminiFinishReason` (`trim` + lowercase + `-`→`_`, with the
   `max_tokens` remap); blank reasons are ignored.
5. Grounding metadata → `google_search` hosted updates emitted **once per metadata change**
   (snapshot comparison), with query joining (` · `), uri dedupe and title fallback.
6. Per-part routing: thought signatures feed `currentThoughtSignature`; string thoughts emit
   `ThoughtChunk`s (with `extractThoughtTitle`: first `**bold**`, else first markdown heading),
   `thought: true` opens a thought block without emitting, `reasoning_content` emits, and text is
   routed as thought or answer depending on the block state; `executableCode` /
   `codeExecutionResult` pair through the pending queue (FIFO keys + arguments, fresh key and
   `"{}"` when the queue is empty, `isError` from the outcome); `functionCall` emits
   `ToolCallUpdate` + `ToolCallRequest` (id fallback `call_<uuid>`, `args` serialization,
   the signature precedence chain, wire-name gates, duplicate-id detection) or the malformed
   stand-in, then resets the signature state.
7. Citations from the line's grounding metadata over the answer text so far (plain `emit` in
   Kotlin — they never count as produced content) and `UsageUpdate` through
   `ApiUsageMetadata.toTokenUsage` (coercions, derived output, derived total).
8. Break conditions: `streamError != null || finishReason != null`.
9. After the loop: `geminiStreamTermination` decides — a non-retryable termination re-emits the
   final citation pass over the full answer, and the termination summary + stream error are the
   last two trace lines.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One transcript per file (`name`, `providerName`, `ops`), 36 frozen edge cases |
| `golden/*.trace` | Frozen canonical output of the Kotlin reference, one per transcript |
| `GeminiStreamTraceDump.kt` | The stand driver (see "How the driver works") |
| ``agora_extract gemini-stream`` | Verbatim extraction of the stream-side provider helpers |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `agora_scenarios gemini-stream <stand>` | Generates the transcripts (deterministic; rerun after editing) |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |

## Transcript format

`ops` is a list of strings, one op per entry:

| Op | Meaning |
| --- | --- |
| `L <line>` | Feed one raw SSE line through the loop (ignored once the loop stopped) |
| `E` | End of stream (`readLine` returned null: the loop exits at its condition) |

## How the driver works

`GeminiProvider.kt` cannot be compiled standalone (HTTP client, coroutines, `DebugLog`), so the
stand pins it in two halves:

* The wire models (`GeminiWireModels.kt`), the citation projection (`GeminiCitationProjection.kt`)
  and the stream event/termination types compile **verbatim from the app sources**, and the
  stream-side provider helpers (thought-title regexes + `extractThoughtTitle`,
  `normalizeGeminiFinishReason`, `geminiStreamTermination`, the hosted-tool projections,
  `JsonObject.stringContent`) are extracted **verbatim** into the generated
  `build/GeminiStreamHelpers.kt` on every run (visibility widened `private` → `internal`).
* The loop body itself is a transcription of `generateResponse`'s 200-branch, over the real
  helpers, the real `malformedToolCallRequest`, the real `ProviderRetryPolicy` and the real
  `carriesModelOutput` rule. The traces pin every line of the transcription.

## Trace format (`golden/*.trace`)

`name|` and `mode|stream` first, then one line per emitted event in emission order, then the two
summary lines. Escaping is the shared `\\` `\p` `\s` `\n` `\r` `\t` set. UUIDs collapse to
`<uuid>` in **both** forms (dashed and compact — malformed stand-in ids carry the compact form).

```
text|<esc>
thought|<esc>|title=<esc|->|signature=<esc|->
toolCallUpdate|streamKey=<uuid>|id=<uuid|->|name=<esc>|args=<esc>|signature=<esc|->
toolCall|id=<uuid>|name=<esc>|args=<esc>|streamKey=<uuid>|signature=<esc|->
hosted|streamKey=<uuid>|name=<esc>|arguments=<esc>|result=<esc|->|error=0/1
citation|v=…;sourceId=…;provider=…;kind=…;title=…;url=…;fileName=…;location=…;providerSourceId=…;excerpt=…;anchors=[{start,end,"text"}]
usage|total=N|input=…|cached=…|cacheWrite=-|uncached=…|output=…|reasoning=…
term|sawDone=0/1|finish=<esc|->|produced=0/1|inFlight=0/1
streamError|none
streamError|api|code=<esc|->|type=<esc|->|message=<esc>
streamError|sseParse|raw=<esc>
```

`term|sawDone=` reads the termination's `sawTerminalMarker` (done **or** finish reason), exactly
like the Kotlin driver. `streamError` records the Api/SseParse shapes; kotlinx cause texts are
not pinned (only `rawLine.take(512)` in UTF-16 units — a split surrogate pair becomes `?`).

## Scope and boundaries

* Transport is out of scope: socket reads, timeouts, cancellation, retry decisions and the HTTP
  statuses. The termination inputs (`timedOut`, retries) travel the same way as in the
  stream-normalization module; the driver pins `toolCallInFlight = pending queue non-empty`
  (the provider's `toolCallInFlight` local is never set — pinned as-is).
* `extractThoughtTitle` replicates the Java regex semantics: `.` excludes only `\n`, and `\s`
  crosses line breaks; the pinned inputs cover both captures.
* `String.uppercase()`/`lowercase()` on finish reasons and outcomes are ASCII-only, consistent
  with the rest of the core port.
* Citation payload geometry comes from the already-pinned `citations` module
  (`to_gemini_citations`); this stand pins WHEN citations fire and over which answer text.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
../../../build/agora_scenarios gemini-stream .   # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the helper functions move or change shape.
