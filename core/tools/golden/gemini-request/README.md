# gemini-request golden stand

Parity pin for the native port of the **Gemini `streamGenerateContent` request pipeline** — the
slice that turns a prepared conversation into the exact wire body. Every scenario under
`scenarios/` is replayed through the **real Kotlin declarations** and the canonical trace is
frozen under `golden/`; the C++ port (`agora::core::gemini`, test
`core/tests/gemini_request_parity_test.cpp`) must reproduce every trace byte for byte.

## Pipeline under test

Mirrors `GeminiProvider.buildApiContents` + `buildRequestBody` and their surroundings, in the
provider's order:

1. Tool assembly — built-in tools first (`code_execution {}`, `google_search {}`), then one
   `function_declarations` entry (typed schemas via `ToolSchemaJson.of`; an external MCP schema
   travels through `parameters_json_schema` instead).
2. `config.resolvedThinking(ThinkingProviderFamily.GEMINI)` → `thinkingConfig`: `disabled`
   (with `thinkingBudget: 0` when the model documents a budget), the `thinkingBudget` token
   form, or the `thinkingLevel` form (uppercased effort); `generationConfig` is always sent.
3. `toolConfig.includeServerSideToolInvocations` only when built-ins AND declarations are both
   present.
4. `adaptToolRoundsForProvider` with `isGeminiToolRoundCompatible` — signatures of a tool round
   must be attributable to the target model+provider (and present at all for `gemini-3*`
   models); a rejected round becomes inert archived context.
5. Message walk — `tool_` rows become the model `functionCall` turn (segments, or the legacy
   `toolCall` shape; a row with neither is always incompatible and archives), `result_` rows
   become the answering `functionResponse` turn (`buildGeminiFunctionResponse` passes parsed
   objects through and wraps everything else as `{"result": ...}`), model rows preserve
   code-execution history (`geminiModelMessageParts`), user rows carry inline images (when
   enabled) or the `[Attachment unavailable]` fallback.
6. `coalesceGeminiContents` — consecutive contents with the same non-null role concatenate.
7. `requireValidWireFormat(cleanModelName)` — the object grammar (roles, alternation,
   `functionResponse` pairing incl. name matching, exactly-one-payload parts, system
   instruction, tool declarations, generationConfig gates, the Gemini 3 signature rule).
8. kotlinx serialization with `Json { ignoreUnknownKeys = true; encodeDefaults = true;
   explicitNulls = false }`, then `Base64FileRegistry.prepare` and the serialized-body gate
   (`contents` array only — Gemini has no model field in the body).

The conversation input is the RESOLVED request input: trimming and prompt resolution
(`prepareMessages`) are pinned by the message-preparation stand. The model id travels in the
URL, so in the body it only reaches the validator (after `removePrefix("models/")`) and the
signature-compatibility check.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One request per file, 73 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `GeminiRequestTraceDump.kt` | The stand driver (see "How the driver works") |
| ``agora_extract gemini-request`` | Verbatim extraction of the request-side provider helpers |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `files/` | Attachment fixtures for the base64 registry |
| `stubs/` | Leaf stubs for Android-only types the reference sources touch |

## How the driver works

`GeminiProvider.kt` cannot be compiled standalone (HTTP client, coroutines, `DebugLog`), so the
stand pins it in two halves:

* `GeminiWireModels.kt` and `GeminiRequestValidator.kt` compile **verbatim from the app
  sources**, and the request-side helpers (`isGeminiToolRoundCompatible`,
  `geminiModelMessageParts`, `JsonObject.stringContent`) are extracted **verbatim** into the
  generated `build/GeminiRequestHelpers.kt` on every run (the only transformation is widening
  `stringContent` from `private` to `internal`).
* `buildApiContents` / `buildRequestBody` are local functions inside `generateResponse`; the
  driver transcribes them line for line on top of the real internals (`adaptToolRoundsForProvider`,
  `coalesceGeminiContents`, `requireValidWireFormat`, `resolvedThinking`, `ToolSchemaJson.of`,
  `requireValidSerializedRequest`, `Base64FileRegistry`). The traces pin every transcription.

## Scenario format (`scenarios/*.json`)

```jsonc
{
  "name": "gemini_tool_round",          // must equal the file stem
  "providerName": "Google",
  "mode": "factory",                    // factory | validate | serialized
  "config": {                           // the ProviderConfig fields the builder reads
    "modelId": "gemini-2.5-pro",        // also drives the validator and signature rules
    "thinkingEnabled": true,
    "thinkingLevel": "medium",
    "thinkingBudgetEnabled": false,
    "thinkingBudgetTokens": 4096,
    "codeExecutionEnabled": false,
    "googleSearchEnabled": false,
    "includeImages": true,
    "temperature": 0.7,                 // number; special values as strings: "NaN", "Infinity"
    "maxTokens": null,
    "topP": null,
    "frequencyPenalty": null,
    "presencePenalty": null,
    "tools": null                       // ToolDefinition wire JSON; a `schema` block becomes
                                        // parameters_json_schema
  },
  "requestInput": {
    "systemPrompt": "You are helpful.",
    "messages": [{
      "id": "user_1",                   // "tool_" / "result_" prefixes select protocol shapes
      "participant": "USER",            // USER | MODEL | ERROR
      "text": "Hello",
      "images": ["files/a.png"],        // paths relative to the stand root
      "modelName": null,                // the stored model of a signed round row
      "segments": null,                 // including signature / signatureProvider
      "toolCall": null                  // the legacy single-call shape
    }]
  }
}
```

`mode: "validate"` feeds `request` (wire-form Gemini JSON) through
`Json { ignoreUnknownKeys = true }.decodeFromJsonElement` and then
`requireValidWireFormat(modelId.removePrefix("models/"))`; it pins validator branches the
converter cannot produce, the kotlinx decode-failure **class names** (missing required fields,
null-into-non-null, wrong kinds — a number never decodes into a string field) and quoted-number
acceptance for the numeric fields.

`mode: "serialized"` calls `requireValidSerializedRequest` with the raw body and the required
field sets.

## Trace format (`golden/*.trace`)

`name|` and `mode|` first; then per mode:

```
contents|<wire ApiRequestContent array, serialized with the pipeline's Json>
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
  loop (`normalizeGeminiFinishReason` / `geminiStreamTermination` belong to a future stream
  stand, as do `toGoogleSearchHostedUpdate` / `toCodeExecutionStart`, which are stream-side
  projections).
* The provider display name is `Google` throughout this stand (tool-round adaptation reasons and
  signature issuer checks); the wire validator hardcodes `"Gemini"` in its exception text, and
  the serialized gate uses the provider display name.
* Signature issuer / model-name comparisons fold case for ASCII only; `String.contains/equals`
  with `ignoreCase` is Unicode-aware, and wire identities are ASCII by construction.
* `geminiModelMessageParts` treats any JSON primitive under `output` as a primitive (including
  `null`, whose content is the string `"null"`) — kotlinx `JsonPrimitive` semantics, pinned.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the helper functions move or change shape.
