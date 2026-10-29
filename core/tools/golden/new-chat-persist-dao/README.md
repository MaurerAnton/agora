# new-chat-persist-dao golden stand

Parity pin for `data/local/NewChatPersistDao.kt`: the `id = 0` singleton draft row behind the New
Chat composer — its observed Flow and suspend read, Room's `@Upsert`, the unconditional delete,
and the null-safe field-matching delete.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/NewChatPersistDao.kt` | the module under test — a plain interface, so **no extraction is needed** |
| `data/local/ChatEntities.kt` | `NewChatPersistEntity` (including its `require(id == SINGLETON_ID)` init), the other entities and the converters; compiles whole, no lifting |
| `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the run row referenced by `messages.runId` |
| `model/ChatMessage.kt` and its model dependencies | the enum types behind the converters |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, named-parameter binding, `@Upsert` as insert-then-update-by-primary-key, and a cold single-emission Flow for the observed query |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the database surface |
| `platform/schema.sql` | the Room-generated DDL: the full 15-column `conversations`, 26-column `messages`, `runs`, and the 6-column `new_chat_persist` singleton |

`observeNewChatPersist` returns `Flow<NewChatPersistEntity?>`. Room's Flow re-runs the query on
invalidation; the stand has no invalidation bus, so its Flow executes the declared SELECT when
collected and emits the current row (or null) once. The C++ port exposes the same row; re-emission
belongs to the caller's store.

## What is pinned

All five members are operations, so no SQL goes unpinned.

* **`observeNewChatPersist` / `getNewChatPersist`** — absent table (`null`), a present row, and
  the row after an upsert, an update and a delete. Both members return the identical projection.
* **`upsertNewChatPersist`** — Room's algorithm: INSERT when the singleton is absent, then
  UPDATE-by-primary-key when it exists, overwriting every non-key column (including clearing
  previously non-null values to NULL). The `require(id == 0)` guard is pinned with `id = 1` and
  `id = -1`, both failing with `IllegalArgumentException: Failed requirement.` before any write.
* **`deleteNewChatPersist`** — returns 1 when the singleton exists and 0 when it does not; the
  table dump confirms the row is gone.
* **`deleteNewChatPersistIfMatches`** — SQLite's null-safe `IS` comparison for `modelId`,
  `systemPromptId`, `conversationSettingsJson` and `draftAttachments`, plus equality on
  `draftText`: a mismatched draft text leaves the row and returns 0; matching NULLs and values
  deletes exactly one row; a repeated call returns 0.
* **Mutation visibility**: every trace ends with a dump of the full `new_chat_persist` row
  (`id, modelId, systemPromptId, conversationSettingsJson, draftText, draftAttachments`).

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"newChatPersist": [{"id": 0, "modelId": "m1", "draftText": "draft"}]},
 "steps": [{"op": "upsertNewChatPersist", "modelId": null, "draftText": "z"},
           {"op": "deleteNewChatPersistIfMatches", "modelId": null, "draftText": "z"}]}
```

Missing optional fields mean NULL; `draftText` defaults to `''`. `agora_scenarios new-chat-persist-dao` writes five
scenarios over purpose-built seeds.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`, where a result is the entity, a row
count, `null`, or `{error, message}`) followed by `tables`, serialized exactly like
`Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=kotlinx-serialization-core.jar:kotlinx-serialization-json.jar:kotlinx-coroutines-core.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # regenerate scenarios and verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
