# openai-request golden stand

Pins the **chat-completions request pipeline** — conversation to wire messages, thinking
customization, kotlinx serialization, wire validation and base64 body preparation — byte for byte
between the Kotlin app sources and the C++ port under `core/`.

Same doctrine as `core/tools/golden/openai` (the SSE consumer stand): the driver compiles the REAL
app sources (no re-implementation), freezes canonical traces, and the C++ core must reproduce them
exactly (`core/tests/openai_request_parity_test.cpp`).

## Pipeline under test

`OpenAiChatRequestFactory.build(config, requestInput)`:

1. `convertToOpenAiMessages` — system prompt (blank dropped), `tool_`/`result_` protocol messages
   (segment or legacy `toolCall` shape), normal messages with images registered as placeholders,
   `[Attachment unavailable]` fallback, `reasoning_content` forwarding.
2. `OpenAiChatRequest` assembly — `stream: true`, `stream_options.include_usage: true`, sampling
   fields, tools, service tier, prompt cache key.
3. The provider hook (`customizeRequest`) — one of the six real hook sets, selected by `kind`.
4. `requireValidWireFormat` — object validation; violations raise `RequestFormatException`.
5. kotlinx serialization with `Json { ignoreUnknownKeys = true; encodeDefaults = true;
   explicitNulls = false }`.
6. `requireValidSerializedRequest` — serialized-body gate.
7. `Base64FileRegistry.prepare` — placeholder replacement descriptor (segments, parts,
   `diagnosticJson`, exact `contentLength`).

## Scenario format (`scenarios/*.json`)

```jsonc
{
  "name": "user_text",                 // trace file name is <name>.trace, must equal the file stem
  "kind": "deepseek",                  // openai | deepseek | groq | qwen | openrouter | custom
  "providerName": "DeepSeek",          // provider identity used in errors and invalid-request text
  "providerBaseUrl": null,             // custom only: defaultBaseUrl for the cheapest-inference rule
  "mode": "factory",                   // factory | validate | serialized | toolid
  "config": {                          // ProviderConfig fields the pipeline reads
    "modelId": "deepseek-v4",
    "thinkingEnabled": true,
    "thinkingLevel": "medium",
    "thinkingBudgetEnabled": false,
    "thinkingBudgetTokens": 4096,
    "includeImages": true,
    "includeAssistantReasoning": false,
    "googleSearchEnabled": false,
    "openAiServiceTier": null,         // string | null
    "temperature": 0.7,                // number; special values as strings: "NaN", "Infinity"
    "maxTokens": null,
    "topP": null,
    "frequencyPenalty": null,
    "presencePenalty": null,
    "promptCacheKey": null,
    "tools": null                      // ToolDefinition wire JSON; a `schema` block is carried
                                       // straight into ToolParameters.schema (see README)
  },
  "requestInput": {
    "systemPrompt": "You are helpful.",
    "messages": [
      {
        "id": "user_1",                // full id; "tool_" / "result_" prefixes select protocol shapes
        "participant": "USER",         // USER | MODEL | ERROR
        "text": "hi",
        "images": ["files/tiny.png"],  // paths relative to the stand root
        "modelName": null,
        "segments": null,              // MessageSegment fields the converter reads
        "toolCall": null               // legacy ToolCallData shape
      }
    ]
  }
}
```

`mode: "validate"` skips the factory and decodes `request` (a wire-form `OpenAiChatRequest`) with a
plain `Json { ignoreUnknownKeys = true }`, then runs `requireValidWireFormat` only. It pins validator
branches the converter can never produce (invalid roles, tool fields on system messages, ...).

`mode: "serialized"` calls `requireValidSerializedRequest` with the raw `serializedBody` string and
`serializedRequiredStringFields` / `serializedRequiredArrayFields`; it pins the serialized-body gate
without a JSON decode of the request.

`mode: "toolid"` calls `buildToolCallId(toolIdName, toolIdArguments, toolIdPrefix)` directly (the
prefix defaults to `call_` when omitted) and records `toolId|<id>`; it pins every name-normalization
branch, including the ones a valid request body can never reach.

## Boundary

`openAiWebSearchEnabled` chat requests are rejected by the provider orchestration before the factory
runs ("hosted web search requires Responses API transport"); headers, endpoint candidates and the
retry loop belong to the transport slice, and the Responses-API branch stays in Kotlin. The factory
slice never sees those paths.

## Trace format (`golden/*.trace`)

One `|`-separated record per line, escaped like the SSE stand (`\\` `\p` `\s` `\n` `\r` `\t`),
with every dashed UUID collapsed to `<uuid>` — base64 placeholders therefore read
`__AGORA_BASE64_<uuid>__`. (Only the dashed form is normalized: the compact 32-hex alternative
would also swallow deterministic hex runs, and the placeholders are the only random data here.)

```
name|<scenario name>
kind|<kind>
mode|<mode>
apiMessages|<wire OpenAiMessage array, serialized with the pipeline's Json>
body|<the exact serialized request body>
diagnostic|<diagnosticJson with [STREAMED_BASE64:<n>_BYTES] markers>   (only with images)
contentLength|<exact wire byte length of the resolved body>           (only with images)
file|<index>|<path relative to the stand>|<bytes>|<sha256 hex>        (per image, wire order)
resolvedBody|<body with every placeholder replaced by base64>         (only with images)
error|<ExceptionSimpleName>|<message>                                 (on failure, replaces the rest)
```

In `validate` mode a passing request records `ok`.

`error` lines carry the exact message for `RequestFormatException` (the provider gate — the C++ port
must format the same text); every other exception records its class name alone, because those texts
belong to the Kotlin runtime and are not part of the port contract.

`resolvedBody` is the substitution `ReplayableJsonRequestBody.writeTo` performs; the app-side
metadata/sha checks of that adapter are covered by `StreamingJsonRequestBodyTest.kt`, and the C++
port owns its own copy of the write loop.

## Files

- `files/` — deterministic fixture payloads (deliberately odd sizes cover base64 padding cases;
  extensions select the MIME type; `missing.png` is intentionally absent).
- `stubs/` — leaf stubs for Android-only types the app sources touch (same idea as the SSE stand).
- `build/` — generated (classes + fresh traces); not committed.

## Requirements

- kotlinc (KOTLINC env or PATH) with the serialization compiler plugin,
- kotlinx-serialization jars matching the app version, colon-separated in `KOTLINX_JARS`.

## Usage

```bash
KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
```

For the C++ side set `AGORA_FIXTURE_OPENAI_REQUEST_DIR` to this directory when running ctest.
