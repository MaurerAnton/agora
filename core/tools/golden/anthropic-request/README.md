# anthropic-request golden stand

Parity pin for the native port of the **Anthropic Messages request pipeline** — the slice that
turns a prepared conversation into the exact `/v1/messages` wire body. Every scenario under
`scenarios/` is replayed through the **real Kotlin declarations** and the canonical trace is
frozen under `golden/`; the C++ port (`agora::core::anthropic`, test
`core/tests/anthropic_request_parity_test.cpp`) must reproduce every trace byte for byte.

## Pipeline under test

Mirrors `AnthropicProvider.buildRequestBody` and its surroundings, in the provider's order:

1. The Anthropic cache gate (an enabled cache with a TTL other than `5m`/`1h` fails the request
   locally before anything is converted).
2. `config.resolvedThinking(ThinkingProviderFamily.ANTHROPIC)` — the shared capability layer
   decides whether the model takes a thinking parameter at all, and which form:
   `disabled`, `enabled+budget`, `adaptive`, or the forced `enabled+budget` of a
   cannot-disable generation.
3. `ToolSchemaJson.of` — the typed `ToolParameters` become the plain JSON Schema object (or an
   external MCP schema passes through verbatim).
4. `adaptToolRoundsForProvider` — a tool round whose opaque thought signatures cannot be
   attributed to the target model+provider is downgraded to inert archived user context
   (`toolRoundAsPlainContext` + `mergeConsecutiveSameRole` + `stripEmptyTurns`, ported in the
   message-preparation module).
5. Message conversion — `tool_` rows become an assistant turn (signed thinking replay +
   `tool_use` blocks, or the legacy `toolCall` shape, or the `Continue` fallback), `result_`
   rows become the answering `tool_result` user turn, normal rows carry user images registered
   as base64 placeholders (`isNotBlank` text gate, `Continue` fallback for empty turns).
6. `coalesceAnthropicMessages` — consecutive same-role turns concatenate.
7. `max_tokens` — explicit value, else `DEFAULT_MAX_TOKENS` (32768) raised above a thinking
   budget by `ANSWER_HEADROOM_TOKENS` (8192), capped by `AnthropicOutputLimits`.
8. Sampling gates — `temperature`/`top_p` are sent only when the model documents sampling AND
   no thinking parameter is present at all; `top_p` additionally stays legal in `0.95..1` with
   thinking (pinned: a *disabled* thinking parameter still drops `temperature`).
9. `requireValidWireFormat` — the object grammar (roles, alternation, tool-result pairing,
   block shapes, thinking configuration, tool schemas).
10. kotlinx serialization with `Json { ignoreUnknownKeys = true; encodeDefaults = true;
    explicitNulls = false }`, then `Base64FileRegistry.prepare` and the serialized-body gate.

The conversation input is the RESOLVED request input (`ProviderRequestInput`): trimming and
prompt resolution (`prepareMessages`) are pinned by the message-preparation stand.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One request per file, 95 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `AnthropicRequestTraceDump.kt` | The stand driver (see "How the driver works") |
| ``agora_extract anthropic-request`` | Verbatim extraction of the wire models and the two thought-signature compatibility helpers from `AnthropicProvider.kt` |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `files/` | Attachment fixtures for the base64 registry (same bytes as the openai-request stand) |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |

## How the driver works

`AnthropicProvider.kt` cannot be compiled standalone (it imports the HTTP client, coroutines and
`DebugLog`), so the stand pins it in two halves:

* The wire models (`AnthropicRequest` … `AnthropicImageSource`) and the two top-level thought-
  signature compatibility helpers are extracted **verbatim** into the generated
  `AnthropicRequestModels.kt` on every run; the only transformation is widening the helpers'
  visibility from `private` to `internal`.
* `buildRequestBody` itself is a local function inside `generateResponse`; the driver transcribes
  it (and the three message-conversion helpers it calls) line for line, on top of the real
  internal utilities (`resolvedThinking`, `adaptToolRoundsForProvider`, `ToolSchemaJson.of`,
  `coalesceAnthropicMessages`, `requireValidWireFormat`, `AnthropicOutputLimits`,
  `requireValidSerializedRequest`, `Base64FileRegistry`). The traces pin every transcription.

## Scenario format (`scenarios/*.json`)

```jsonc
{
  "name": "factory_thinking_adaptive",   // must equal the file stem
  "providerName": "Anthropic",
  "mode": "factory",                     // factory | validate | serialized
  "config": {                            // the ProviderConfig fields the builder reads
    "modelId": "claude-opus-4-6",
    "thinkingEnabled": true,
    "thinkingLevel": "medium",
    "thinkingBudgetEnabled": false,
    "thinkingBudgetTokens": 4096,
    "includeImages": true,
    "anthropicCacheEnabled": true,
    "anthropicCacheTtl": "1h",
    "temperature": 0.7,                  // number; special values as strings: "NaN", "Infinity"
    "maxTokens": null,
    "topP": null,
    "tools": null                        // ToolDefinition wire JSON (a `schema` block is
                                         // carried straight into ToolParameters.schema)
  },
  "requestInput": {
    "systemPrompt": "You are helpful.",
    "messages": [{
      "id": "user_1",                    // "tool_" / "result_" prefixes select protocol shapes
      "participant": "USER",             // USER | MODEL | ERROR
      "text": "Hello",
      "images": ["files/a.png"],         // paths relative to the stand root
      "modelName": null,                 // the stored model of a signed thought row
      "segments": null,                  // including signature / signatureProvider
      "toolCall": null                   // the legacy single-call shape
    }]
  }
}
```

`mode: "validate"` feeds `request` (wire-form Anthropic JSON) through
`Json { ignoreUnknownKeys = true }.decodeFromJsonElement` and then `requireValidWireFormat` only;
it pins validator branches the converter cannot produce, the kotlinx decode-failure **class
names** (missing required fields, null-into-non-null, wrong kinds, number grammar) and
last-wins duplicate decoding.

`mode: "serialized"` calls `requireValidSerializedRequest` with the raw body and the required
field sets.

## Trace format (`golden/*.trace`)

`name|` and `mode|` first; then per mode:

```
apiMessages|<wire AnthropicMessage array, serialized with the pipeline's Json>
body|<the exact serialized request body>
diagnostic|<diagnosticJson with [STREAMED_BASE64:<n>_BYTES] markers>   (only with images)
contentLength|<exact wire byte length of the resolved body>           (only with images)
file|<index>|<path relative to the stand>|<bytes>|<sha256 hex>        (per image, wire order)
resolvedBody|<body with every placeholder replaced by base64>         (only with images)
ok                                                                      (passing validate/serialized)
error|<ExceptionSimpleName>|<message>                                  (replaces the rest)
```

`error` lines carry the exact message for `RequestFormatException` (the provider gate — the C++
port formats the same text) and the JVM class simple name alone for every other exception
(`JsonDecodingException`, `MissingFieldException`, `JsonEncodingException`), because those texts
belong to the Kotlin runtime and are not part of the port contract. UUIDs (base64 placeholders)
are collapsed to `<uuid>`; escaping is the shared `\\` `\p` `\s` `\n` `\r` `\t` set.

## Scope and boundaries

* Transport is out of scope: the retry loop, HTTP statuses, headers, `fetchModels` and the read
  loop (pinned by the anthropic-stream stand).
* The provider display name is `Anthropic` throughout this stand; the wire validator hardcodes
  it in its exception text, and the tool-round adaptation reasons and signature issuer checks
  use it (case-insensitively).
* Signature issuer / model-name comparisons fold case for ASCII only; `String.equals(ignoreCase)`
  is Unicode-aware, and wire identities are ASCII by construction.
* The `signatureProvider`-less legacy shapes, `Continue` fallbacks, archived-context digests
  (SHA-256 of the round) and every other conversion branch are pinned by the factory scenarios.
* Lone `result_` rows are dropped by the tool-round adapter; that removal is pinned.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the model declarations or the compatibility helpers move or change shape.
