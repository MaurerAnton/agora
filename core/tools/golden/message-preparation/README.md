# message-preparation golden stand

Pins the **provider-neutral message-preparation pipeline** — the transformations a conversation
goes through between durable history and the provider request — byte for byte between the Kotlin
app sources and the C++ port under `core/`.

Same doctrine as the other stands: the driver compiles the REAL Kotlin sources (no
re-implementation), freezes canonical traces, and the C++ core must reproduce them exactly
(`core/tests/message_preparation_parity_test.cpp`).

## Pipeline under test

The shared "fail-closed" preparation every provider receives:

1. `canonicalContextMessages` — Compact projection (`applyNearestContextCompact`: nearest
   successful `compact_` row becomes a synthetic `context_summary_` USER row), id deduplication,
   `validateToolMessages` (tool-round normalization or `protocol_notice_` fallback), empty-turn
   stripping and same-role merging.
2. `prepareMessages` — the request-boundary pipeline: the API-initial-user prompt row detection,
   `<context_summary>` markup with the optional "Please continue." continuation, `limitContext`
   trimming by token budget, a final merge/strip and the prompt row re-append.
3. `ContextWindow` helpers: `protocolAtomicUnits`, `splitContextForCompactRetention`,
   `splitLogicalContext`, `contextWindowUsage` and `contextWindowRetainedMessageIds`.
4. `CostModelContextEstimator` — the message walk (`estimate`), the fixed cost
   (`estimateFixed`, `estimateFixedComposition`) and `estimateText`, priced through a
   `ContextCostModel` (heuristic or real o200k BPE, per-family envelopes, safety margin).
5. The `ToolMessages` API projections: `projectAssistantImagesToLatestUserMessage`,
   `projectToolResultImagesToUserMessage` and `projectGenerationStatusesForApi`.

## Boundary

- The viewmodel policy layer that wraps the same boundary for dispatch
  (`projectGenerationInputMessages` and the user/assistant template application in
  `GenerationPolicies.kt`) stays Kotlin with the UI layer; the stand pins everything it delegates
  to. The provider-specific adapters (`adaptToolRoundsForProvider`, Anthropic/Gemini signature
  handling) belong to those providers' own slices.
- The embedded-runtime family (`ContextCostFamily.LOCAL`) cannot be pinned on the host: the stub
  `LocalModelRuntime` answers "no exact count" like the app does before a model is resident, and
  no scenario selects a Local model id.

## Stubs (`stubs/`)

- `O200kBase` — the Android asset loader replaced by a host-file loader over the SAME shipped
  `o200k_base.tiktoken` vocabulary (path injected by the driver); `nativeCounter()` is always
  absent, exactly the app's state with the native library down, so counting runs through the
  real Kotlin `BpeVocabulary`/`BpeTokenCount` sources compiled alongside.
- `LocalModelRuntime` — answers null for exact counts (see Boundary).
- `androidx.compose.runtime.Immutable/Stable`, `com.mikepenz.markdown.model.State` — compile-only
  annotations/markers for the durable model.

## Scenario format (`scenarios/*.json`)

```jsonc
{
  "name": "prepare_trims_by_budget",   // trace file name is <name>.trace, must equal the file stem
  "mode": "prepare",                   // full list below
  "budget": 30,                        // token budget for limit/usage/retained/prepare
  "includeAssistantReasoning": false,  // thought segments count toward the estimate
  "fixedTokenCost": 0,                 // system prompt + tools cost added before trimming
  "costModel": null,                   // "Provider:model" priced via ContextCostModels; null = Default (heuristic)
  "systemPrompt": null,                // estimateFixed modes
  "initialUserPrompt": null,
  "codeExecutionEnabled": false,
  "googleSearchEnabled": false,
  "openAiWebSearchEnabled": false,
  "tools": null,                       // ToolDefinition wire JSON, same shape as the request stand
  "fixedSystemPromptTokens": 0,        // usage mode: the UI fixed composition
  "fixedToolTokens": 0,
  "retainMessages": 0,                 // splitCompact
  "retainLogicalMessages": 0,          // splitLogical
  "includeImages": true,               // projection modes
  "text": "",                          // estimateText
  "messages": [
    {
      "id": "tool_1",                  // REQUIRED; "tool_" / "result_" / "compact_" prefixes matter
      "parentId": null,
      "participant": "MODEL",          // USER | MODEL | ERROR
      "status": "SUCCESS",             // SUCCESS | ERROR | STOPPED | ...
      "text": "",
      "images": [],
      "thoughts": null,
      "thoughtTitle": null,
      "tokenCount": 0,
      "modelName": null,
      "timestamp": 0,                  // deterministic; the app default is "now"
      "runId": null,
      "runSequence": null,
      "segments": null,                // MessageSegment fields (tool rows, thoughts, errors)
      "toolCall": null,                // legacy ToolCallData shape
      "attachmentMeta": null           // attachment items: type/index/pages/bytes/pixels
    }
  ]
}
```

## Modes

| mode | calls | trace |
| --- | --- | --- |
| `prepare` | `prepareMessages` | one `msg` line per output message |
| `compactProjection` | `applyNearestContextCompact` | `msg` lines |
| `canonical` | `canonicalContextMessages` | `msg` lines |
| `strip` / `merge` / `validateTools` | the single transform | `msg` lines |
| `limit` | `limitContext` | `msg` lines |
| `units` | `protocolAtomicUnits` | `unit\|<ids>` per unit |
| `usage` | `contextWindowUsage` | `usage\|estimated\|budget\|logical\|compact\|system\|tools\|messages\|progress` |
| `retained` | `contextWindowRetainedMessageIds` | `retained\|<ids>` |
| `splitCompact` | `splitContextForCompactRetention` | `prefix\|<ids>`, `retained\|<ids>`, `count\|<n>` |
| `splitLogical` | `splitLogicalContext` | `prefix\|<ids>`, `suffix\|<ids>`, `count\|<n>` |
| `estimate` | `CostModelContextEstimator.estimate` | `estimate\|<tokens>` |
| `estimateFixed` / `fixedComposition` | the fixed cost | `fixed\|<tokens>` / `fixedc\|<system>\|<tools>` |
| `estimateText` | one string | `text\|<tokens>` |
| `projectAssistantImages` / `projectToolResultImages` | the image projections | `msg` lines |
| `projectStatuses` | `projectGenerationStatusesForApi` (fixed `"formatted:"` detail formatter) | `msg` lines |

## Trace format

One `|`-separated record per line, escaped like the other stands (`\\` `\p` `\s` `\n` `\r` `\t`):

```
name|<scenario>
mode|<mode>
msg|<index>|<id>|<parentId|->|<PARTICIPANT>|<STATUS>|<text>|<images,comma-joined>|<modelName|->|<thoughts|->|<timestamp>|<runId|->|<runSequence|->|<segmentsJson>|<toolCallJson|->|<attachmentsJson>
```

`segmentsJson` is the JSON array of segment dumps (every field the pipeline reads or writes,
`responseOutputItems` as a count); `toolCallJson` is one tool-call dump or `-`; `attachmentsJson`
is the attachment-item array. `usage`/`estimate*`/`unit`/`retained`/`prefix`/`suffix`/`count`
lines carry raw numbers or comma-joined ids (already scenario-controlled, escaped as usual).

An unexpected exception prints `error|<JVM simple class name>` only; there is no format gate in
this module whose message text is part of the contract.
