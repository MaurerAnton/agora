# embedding golden stand

Parity pin for the native port of the EmbeddingClient's encoding and response parsing. Every
scenario under `scenarios/` runs through the **real Kotlin declarations** and the canonical
trace is frozen under `golden/`; the C++ port (`agora::core::embedding`, test
`core/tests/embedding_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 29 frozen edge cases across four modes |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `EmbeddingTraceDump.kt` | The stand driver (parsing transcription over the real models) |
| `extract_embedding_models.py` | Verbatim extraction of the request models |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## How the stand works

`EmbeddingClient.kt` wires coroutines, `HttpClient` and `DebugLog` and cannot compile
standalone, so the stand pins it in two halves:

* `extract_embedding_models.py` copies the `@Serializable` request models
  (`EmbeddingRequest`, `BatchEmbeddingRequest`) **verbatim** (visibility widened `private` →
  `internal`); the request modes serialize them with the real kotlinx serializers.
* The response parsing (`computeEmbedding` / `computeEmbeddingResponse` halves) is a
  line-for-line transcription in the driver: the `try/catch` failure surface, the early
  returns, and the per-item `?: return@map null` are all mirrored. The HTTP call, url and
  headers are the transport boundary.

## Scenario format and modes

`{name, mode, text, texts, model, response}`:

| mode | Calls | Trace lines |
| --- | --- | --- |
| `requestSingle` | serialize `EmbeddingRequest` | `body\|<json>` |
| `requestBatch` | serialize `BatchEmbeddingRequest` | `body\|<json>` |
| `single` | `computeEmbedding`'s parse | `embedding\|0\|<values\|none>` |
| `batch` | `computeEmbeddings`' parse | `embedding\|<i>\|<values\|none>` per item |

Empty `texts` in `batch` mode produce **no** lines (the Kotlin early return precedes the
response handling), and the batch result length follows the **response's** data array, not the
request (pinned: 3 texts / 2 items → 2 lines).

## Pinned Kotlin/Java semantics

* Float rendering is `Float.toString()` — `0.1`, `1000000.0`, `1.0E-8`, `-0.0` and
  `Infinity` are all pinned (non-finite JSON values decode; `format_kotlin_float` mirrors the
  JVM spelling).
* Quoted number elements (`"1.5"`) decode (JsonPrimitive.content semantics); a non-primitive
  element or a non-numeric value fails the whole parse (their exceptions escape the map).
* An absent `embedding` key is the only per-item miss; an explicit `"embedding": null` is a
  present `JsonNull`, whose `.jsonArray` throws — **all-nulls**, not a per-item miss (pinned).
* Request bodies pin kotlinx field order (`input` before `model`) and string escaping.

## Scope and boundaries

Transport is out of scope: `/embeddings` url building, `Authorization` headers, the POST, and
the `DebugLog` calls. Only the pure encode/parse halves are pinned.

## Regenerating

```
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                          # rewrite scenarios
```

The Kotlin reference is compiled from the app tree on every run; the extractor fails loudly
if the request models move or change shape.
