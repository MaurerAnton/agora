# ollama golden stand

Parity pin for the native port of the **Ollama provider** — both halves of `OllamaProvider`:
the chat request pipeline and the NDJSON stream loop. Every scenario under `scenarios/` is
replayed through the **real Kotlin declarations** and the canonical trace is frozen under
`golden/`; the C++ port (`agora::core::ollama`, test `core/tests/ollama_parity_test.cpp`)
must reproduce every trace byte for byte.

## What is pinned

**Request half** (mode `factory`):

1. `buildApiMessages` — the system row (non-blank `systemPrompt` first), `tool_` rows becoming
   assistant turns with `tool_calls` (segments or the legacy `toolCall` shape, `thinking` from
   the **last** thought segment, `Continue`-free: a `tool_` row with neither shape contributes
   nothing), `result_` rows becoming per-segment `tool` rows, normal rows with registered
   images (user rows only, `includeImages` gate, missing files filtered).
2. The `options` object (temperature → top_p → num_predict, omitted when empty; a non-finite
   temperature is rejected at **encode** time with `JsonEncodingException`, after validation).
3. The `think` value from `config.resolvedThinking(OLLAMA)`: the effort string (`low`/`medium`/
   `high`/`max`) when the model documents one (`gpt-oss`), otherwise the enabled boolean.
4. `requireValidWireFormat` — the full object grammar (roles incl. the system-row position,
   pending tool-name pairing FIFO, the shared `validateToolDefinitions`, the `think` shape
   gates, history edges) with the hardcoded `"Ollama"` prefix in its exception text.
5. kotlinx serialization with `Json { ignoreUnknownKeys = true; encodeDefaults = true;
   explicitNulls = false }` (`content` always serializes), then `Base64FileRegistry.prepare`
   and the serialized-body gate (`model` string + `messages` array, provider `"Ollama"`).

**Stream half** (mode `stream`): raw NDJSON lines (there is **no** `data: ` prefix — a plain
garbage line is a parse failure), `done_reason` (trim + non-empty + lowercase into the
termination `stopReason`), the in-band `error` field (`type=ollama_stream_error`), the batched
tool-call snapshot handling (updates always emitted, then a single request or a
`ToolCallsRequest`; wire-name gates, argument-object check, duplicate ids inside the batch
→ malformed stand-in; string arguments pass as content, primitives as their lexeme), thinking
fields, content chunks, `done` → usage through `toTokenUsage` (coercions, no cache/reasoning
split), the break conditions and the termination summary.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One transcript per file, 69 frozen edge cases across four modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `OllamaTraceDump.kt` | The stand driver (see "How the driver works") |
| ``agora_extract ollama-models`` | Verbatim extraction of the wire models and loop helpers |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `agora_scenarios ollama <stand>` | Generates the scenarios (deterministic; rerun after editing) |
| `files/` | Attachment fixtures for the base64 registry |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |

## How the driver works

`OllamaProvider.kt` cannot be compiled standalone (HTTP client, coroutines, `DebugLog`), so
the stand pins it in two halves:

* The wire models (`OllamaChatRequest` … `OllamaStreamResponse`) and the loop helpers
  (`ollamaStreamTermination`, `OllamaStreamResponse.toTokenUsage`) are extracted **verbatim**
  into the generated `build/OllamaModels.kt` on every run; the validator
  (`OllamaRequestValidator.kt`) and every app utility (converters, registries, validators,
  thinking resolution, stream events/termination) compile **verbatim from the app sources**.
* `buildApiMessages`, the options/think assembly, `buildRequestBody` and the NDJSON loop are
  local functions inside `generateResponse`; the driver transcribes them line for line over
  the real internals. The traces pin every transcription.

## Scenario format (`scenarios/*.json`)

`{name, mode, config?, requestInput?, request?, serializedBody(+fields)?, ops?}` with
`mode: factory | validate | serialized | stream`:

* `factory` — conversation + config → the pipeline above (trace lines `apiMessages|`, `body|`
  and the streaming lines when images register).
* `validate` — wire-form request JSON through
  `Json { ignoreUnknownKeys = true }.decodeFromJsonElement` and `requireValidWireFormat` only;
  pins validator branches the converter cannot produce plus the decode-failure **class names**.
* `serialized` — `requireValidSerializedRequest` with the raw body and required fields.
* `stream` — `ops`: `"L <raw ndjson line>"` feeds one line through the loop (ignored once
  stopped), `"E"` ends the stream.

The provider name is the hardcoded `"Ollama"` in both gates — the scenario carries no
provider identity.

## Trace format (`golden/*.trace`)

`name|` and `mode|` first, then per mode:

```
apiMessages|<wire OllamaMessage array>
body|<the exact serialized request body>
diagnostic| / contentLength| / file|<i>|<path>|<bytes>|<sha256> / resolvedBody|   (images only)
ok                                                                             (passing validate/serialized)
error|<ExceptionSimpleName>|<message>                                           (replaces the rest)

text|<esc>
thought|<esc>|title=<esc|->|signature=<esc|->
toolCallUpdate|streamKey=<uuid>|id=<uuid|->|name=<esc>|args=<esc>|signature=<esc|->
toolCall|id=<uuid>|name=<esc>|args=<esc>|streamKey=<uuid>|signature=<esc|->
toolCalls|calls=<id~name~args~streamKey;...>
usage|total=N|input=…|cached=…|cacheWrite=-|uncached=…|output=…|reasoning=…
term|sawDone=0/1|stop=<esc|->|produced=0/1|inFlight=0/1
streamError|none|api|sseParse
```

`error` lines carry the exact message for `RequestFormatException` and the JVM class simple
name alone for everything else (`JsonDecodingException`, `JsonEncodingException`,
`MissingFieldException`). UUIDs collapse to `<uuid>` in both forms (dashed and compact — the
malformed stand-in ids carry the compact form).

## Scope and boundaries

* Transport is out of scope: the missing-base-URL `Configuration` guard, headers, retry loop,
  HTTP statuses and `fetchModels`. The loop's `toolCallInFlight` local is **never set** by the
  provider — pinned as always-false in the termination line.
* `isBlank` gates, `mapNotNull` filtering and `ifEmpty` thinking all use the shared Kotlin
  semantics ports (`json::is_blank` etc.); regex wire-name gates are byte-counted ASCII.
* Scenario floats spell non-finite values as strings (`"NaN"`), mirroring the other stands.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
../../../build/agora_scenarios ollama .   # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly
if the model declarations or loop helpers move or change shape.
