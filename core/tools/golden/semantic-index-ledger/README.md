# semantic-index-ledger golden stand

Parity pin for `data/local/SemanticIndexLedger.kt`: the ledger/work admission rules, the exact
and reconcile invalidation cycles, the reconcile page filters, and the four ChatDatabase
orchestrations (`withSemanticSourceMutation`, `withSemanticGraphMutation`,
`withSemanticEligibilityMutation`, `commitSemanticEmbedding`).

`SemanticIndexLedger.kt` and `util/Constants.kt` compile **verbatim from the app tree**;
`data/local/ChatEntities.kt` contributes `EmbeddingEntity` byte for byte (lifted by
`agora_extract embedding-entity`).

## The platform boundary

Room's annotation processor is the replaced platform. Room compiles `@Query`/`@Insert`/`@Upsert`
into generated DAO classes over SQLite; that generated code is not application logic, so the stand
replaces it with two documented pieces:

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | Reflective executor: reads the `@Query` SQL from the RUNTIME-retained stand annotations, binds Kotlin parameter names (expanding collections like Room's `appendPlaceholders`), implements `@Insert`/`@Upsert` with Room's documented algorithm (insert, on a constraint conflict update by primary key), runs `@Transaction` methods in the transaction stack, maps rows back onto the entity data classes |
| `platform/schema.sql` | The Room-generated DDL: the two ledger tables, the `embeddings` table (unique `(messageId, modelId)`, `ON DELETE CASCADE`), and the observable subset of `messages`/`conversations` that the ledger queries (documented there) |

The SQL text itself is never duplicated: it is read from the compiled application source. The
ledger, the fingerprint and every conditional sequence execute for real against SQLite (xerial
JDBC), so the frozen traces pin SQL semantics and Kotlin orchestration together.

Also stand-local: `platform/Room.kt` (annotation shapes), `platform/RoomDatabase.kt`
(`withTransaction` with Room's nesting contract: every level must be marked successful, an
unmarked level rolls the whole stack back) and `platform/StandChatDatabase.kt` (the three-line
`ChatDao` surface the eligibility mutation reads, with the app's own query text).

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 13 scenarios, 115 steps |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `SemanticIndexLedgerTraceDump.kt` | Driver: seeds SQLite, runs the steps, dumps the tables |
| `agora_extract embedding-entity` | Lifts `EmbeddingEntity` out of `ChatEntities.kt` |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Entity validation**: `SemanticIndexLedgerEntity`/`SemanticIndexWorkEntity` init blocks reject
  blank ids, unknown states, out-of-range revisions and inconsistent claim/revision pairs with
  `IllegalArgumentException("Failed requirement.")`.
* **`admitModel` / `requestReconcile` / `enqueueExactWork` / `completeExactWork` /
  `completeReconcile`**: the `@Transaction` bodies, including the `check`/`checkNotNull`
  messages when a ledger row vanishes mid-transaction.
* **`@Upsert` semantics**: insert first; on a primary-key conflict update the row with the same
  key (the auto-generated key `0` updates nothing, exactly like Room's generated adapter).
* **Pagination**: `getWorkPage`'s keyset order `(sourceRevision, messageId)` and
  `getWorkCountThroughRevision`'s revision ceiling.
* **Reconcile eligibility**: task conversations, non-`USER`/`MODEL` participants, empty text,
  `tool_`/`result_`/`compact_` ids and per-message work fencing all exclude a row; the
  `LEFT JOIN embeddings` metadata (`embeddingId`, fingerprint, `dimension`, `LENGTH` of the blob)
  is returned verbatim.
* **Fingerprints**: `semanticSourceFingerprint` truncates to 8000 UTF-16 units and hexes
  SHA-256; a surrogate pair split at the cut renders as `?` like the JVM's UTF-8 encoder.
* **Orchestration**: source mutations enqueue exact work for the active model and reconcile the
  others only when the fingerprint actually changed; graph mutations delete embeddings before
  requesting reconcile; eligibility mutations react only to a real `taskId` transition;
  `commitSemanticEmbedding` re-reads the fingerprint and the work row inside the transaction.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"messages": [{"id": "m1", "conversationId": "c1", "text": "alpha",
                        "participant": "USER"}]},
 "steps": [{"op": "enqueueExactWork", "modelId": "model", "messageId": "m1",
            "sourceFingerprint": "fp", "updatedAt": 10}]}
```

`seed` fills `conversations`, `messages` and `embeddings` (blobs as hex). Steps cover every DAO
method plus the five orchestration ops; `blockSql` entries run inside the surrounding
transaction; an op that raises a Kotlin validation error records
`{"error": "<JVM class>", "message": "<message>"}`.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`) followed by `tables`, a full dump
of `semantic_index_ledger`, `semantic_index_work`, `embeddings`, `messages` and `conversations`
in declared column order, serialized exactly like `Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar:coroutines.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate
python3 gen_scenarios.py                                # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
