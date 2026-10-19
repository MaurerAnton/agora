# embedding-selector golden stand

Parity pin for the native port of `tool/BoundedSemanticEmbeddingSelector.kt`. Every scenario
under `scenarios/` runs through the **real Kotlin selector compiled verbatim from the app tree**
and the canonical trace is frozen under `golden/`; the C++ port
(`agora::core::embedding::BoundedSemanticEmbeddingSelector`, test
`core/tests/embedding_selector_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 35 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `SelectorTraceDump.kt` | The stand driver (calls the real selector through its suspend entry point) |
| `run_golden.sh` | Compiles the app sources and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The selector scores one keyset page at a time and retains only the best `limit` candidates, so a
large embedding cache never has to be held in memory:

* **Bounded retention** — a worst-first heap of size `limit`. Overflow discards the worst
  candidate: lowest score, and on a score tie the **highest** row id. The survivors come back
  sorted by score descending, then row id ascending.
* **Malformed rows are skipped, not fatal** — wrong dimension, non-positive dimension, or a byte
  count that disagrees with the declared dimension. `skippedInvalidRows` counts them.
* **Non-finite scores are skipped too.** JSON has no NaN literal, so the stand reaches that branch
  by overflow: squaring `Float.MAX_VALUE` yields Infinity, and Infinity / Infinity is NaN.
* **`bestScore` is a maximum over every scored row**, taken *before* the threshold test, so a row
  below the threshold still raises it.
* **The keyset contract is enforced** — a page that is not strictly ordered by id, or larger than
  the requested bound, fails with its own message.
* **Early exits** — an empty query or a non-positive limit returns an all-zero selection without
  ever calling the loader; a non-positive `page_size` fails at construction.

## Scenario format (`scenarios/*.json`)

| Field | Meaning |
| --- | --- |
| `name` | scenario identifier; must equal the file stem |
| `page_size` | selector page bound (constructor argument) |
| `query`, `threshold`, `limit` | the `select` arguments |
| `rows` | flat keyset rows; the loader pages them the way the database would |
| `raw_pages` | verbatim pages returned regardless of the cursor (drives the ordering checks) |
| `loader` | `keyset` (default) \| `raw` \| `oversized` |

A row is `{id, message_id, dimension, embedding}`; `dimension = -1` means "the query's size".
Row ids must be `>= 1` and strictly increasing within a scenario: the cursor starts at `0` and the
loader drops ids `<=` it, so the selector's own ordering check would fire otherwise.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
page_size|<n>
threshold|<kotlin float>
limit|<n>
query|<comma-joined kotlin floats>
candidates|<n>
candidate|<rowId>|<messageId>|<score>
scanned|<n>
skipped|<n>
best_score|<float>
max_retained|<n>
error|<escaped message>      (instead of the block above, when the selector throws)
```

Floats are spelled exactly as Kotlin's `Float.toString()`; the C++ side reuses
`openai::format_kotlin_float`. Vectors are encoded big-endian, matching
`ByteBuffer.order(BIG_ENDIAN)`.

## Scope and boundaries

* Only `EmbeddingSearchRow.kt` and `BoundedSemanticEmbeddingSelector.kt` are compiled, both
  verbatim from the app tree. Nothing in the stand reimplements the logic under test.
* The `select` entry point is `suspend`, so the driver drives it with `runBlocking` from the
  kotlinx-coroutines jar kotlinc ships.
* The loader callback stands in for the database; no SQLite, no Room, no Android.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                  # rewrite scenarios
```

`run_golden.sh` wipes the stand's `build/` (its git-ignored scratch) before each run, so runs are
idempotent. Note that kotlinc ≤ 1.9 cannot read the class file version of the JDK it runs on
(it aborts parsing "27"), hence the explicit `JAVA_HOME`.
