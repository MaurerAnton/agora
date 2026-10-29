# mcp-protocol golden stand

Parity pin for the native port of the MCP protocol core: the tool model and its schema
projections (`McpModels.kt`), the call-payload parser and the protocol decisions around the
transport seam (`McpProtocolClient.kt`). Every scenario under `scenarios/` runs through the
**real Kotlin declarations** and the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::mcp`, test `core/tests/mcp_protocol_parity_test.cpp`) must reproduce every trace
byte for byte.

Transport (`McpClientTransport.kt`), session lifecycle, mutexes and the registry
(`McpRegistry.kt`) stay in Kotlin — see **Scope**.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 64 frozen edge cases across eight modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `McpProtocolTraceDump.kt` | The stand driver (real pieces + four transcribed helpers) |
| ``agora_extract mcp-pieces`` | Verbatim extraction from `McpModels.kt` + `McpProtocolClient.kt` |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |
| `stubs/` | Leaf stubs for Android-only types the model sources touch |

## How the stand works

`McpModels.kt` cannot be compiled next to the extracted copies (Kotlin reports conflicting
overloads for the file-private projections), so the stand extracts **every** declaration it
needs verbatim from both files — the tool model, `publicMcpToolName`, `asToolDefinition`, the
four schema projections, `asObjectOrNull`, the protocol constants, the version classifier and
`parseCallPayload`. The only transformation is the member indentation and `private` → `internal`.

Four small helpers are inline in transport-coupled functions and are **transcribed line for
line** by the driver (and mirrored by the port):

| driver helper | source |
| --- | --- |
| `validateRpcResponse` | `requestLocked`, lines 225-237 |
| `negotiationCandidates` | `ensureInitializedLocked`, lines 132-137 |
| `toolsPage` | `listTools`, lines 79-101 |
| `negotiatedVersion` | `initializeLocked`, lines 176-180 |

## Modes

| mode | Calls | Trace |
| --- | --- | --- |
| `toolName` | `publicMcpToolName` | `name\|<result>` |
| `toolDefinition` | `asToolDefinition` | `definition\|<ToolDefinition JSON>`, `schema\|<wire schema JSON>` |
| `callPayload` | `parseCallPayload` | `text\|<i>\|<esc>` …, `image\|<i>\|<data>\|<mime>` …, `structured\|<json\|->`, `isError\|0/1` |
| `toolsPage` | `listTools` page mapping | `tool\|<i>\|<name>\|<desc>\|<schema>` …, `cursor\|<esc\|->` |
| `rpcResponse` | `requestLocked` validation | `result\|<json>` or `error\|IOException\|<message>` |
| `negotiation` | candidate order | `candidates\|<a,b,c>` |
| `unsupportedVersion` | `isUnsupportedMcpProtocolVersion` | `unsupported\|0/1` |
| `initialize` | `initializeLocked` validation | `negotiated\|<v>`, `sameAsRequested\|0/1` |

## Pinned Kotlin semantics

* `publicMcpToolName` iterates **UTF-16 code units** (`Char`), so a surrogate pair is filtered
  out of the server key and becomes **two** underscores in the tool key (pinned with an emoji);
  the tool key is cut to fit the 64-character budget in UTF-16 units, `filter` uses
  `Char.isLetterOrDigit` (the port uses the generated Unicode tables), and the SHA-256 digest is
  the first three bytes in lowercase hex.
* `toToolParameters` sorts the properties by key, keeps only `required` entries that are declared
  and prune non-primitives, defaults the type to `object`, and infers `object`/`array`/`string`
  per property when no usable type is declared (type unions pick the first non-`null` entry).
* `toWireSchema` copies the server schema verbatim, then drops `$schema` and puts `type`,
  `properties` and (only when present) `required` — in place for keys that already existed,
  appended otherwise (LinkedHashMap rules, pinned by the `schema|` line).
* `asToolDefinition` blanks-description fallback, server banner and property projections.
* `parseCallPayload` keeps blank-free `data`/`mimeType` image pairs, prefers `blob` + `image/*`
  resources over inline text, drops an explicit `structuredContent: null`, and `isError` is a
  strict `true`/`false` spelling.
* `listTools`' page mapping skips non-objects and blank names, defaults a missing `inputSchema`
  to `{"type":"object","properties":{}}`, and a blank `nextCursor` ends pagination.
* `requestLocked` validates in order: absent response, id mismatch (`id` as text), an error
  envelope (`error.message` or the element's JSON text), then a `result` **object** — each with
  its exact message.
* Negotiation drops protocol versions before the current one and wraps to the full list when the
  current version is unknown; SSE always pins `2024-11-05`.
* `isUnsupportedMcpProtocolVersion` joins the cause chain's messages with spaces, lowercases and
  matches three phrases.

## Scope

Transport is out of scope: `McpClientTransport.kt` (stdio/HTTP sessions), the coroutine mutex
and retry loop, `McpRegistry.kt` (server bookkeeping, viewmodel glue) and the `data` enums. The
pinned surface is the pure decision/parsing half the protocol client performs around them.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly if
the declarations move or change shape.