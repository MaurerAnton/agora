# chat-dao golden stand

Parity pin for `data/local/ChatDao.kt`: the aggregate Room DAO that composes the conversation,
run, message, task, loop, search, media-reference, provider-context and New Chat draft surfaces.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatDao.kt` | the module under test — a plain interface, so **no extraction is needed** |
| `data/local/ChatDaoRuntimeRecovery.kt` | the recovery extension invoked by `recoverConversationRuntime` |
| `data/local/ChatContextCompactDao.kt` | the compact/run/message primitives called from `@Transaction` bodies |
| `data/local/ChatAutomationDao.kt` | inherited task and loop persistence |
| `data/local/ChatProviderContextDao.kt`, `ChatSearchDao.kt`, `ChatMediaReferencesDao.kt` | the other inherited DAO surfaces |
| `data/local/NewChatPersistDao.kt` | the singleton New Chat draft row used by conversation creation |
| `data/local/ChatEntities.kt`, `RunEntity.kt` | every Room entity, converter, and validation block |
| `data/local/MessageContextTopology.kt`, `RunBranchSelectionIntegrity.kt`, `ForkEmbeddingClonePolicy.kt`, `EmbeddingSearchRow.kt` | the topology, selection, fork-clone and embedding projections |
| `model/*.kt`, `util/Constants.kt` | enums, runtime values and policies referenced by the entities |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: declared `@Query` SQL against real SQLite, named-parameter binding in SQL-text appearance order, `@Insert`/`@Upsert`/`@Update` generation, collection inserts returning `LongArray`, and cold single-emission Flows |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the database surface |
| `platform/schema.sql` | the Room-generated schema 36 DDL, copied from `app/schemas/.../36.json` |
| `ChatDaoTraceDump.kt`, `ChatDaoTraceSeeding.kt`, `ChatDaoTraceEntities.kt`, `ChatDaoTraceOperations.kt`, `ChatDaoTraceRendering.kt`, `ChatDaoTraceTableDump.kt` | the driver: seeds through the DAO's own upserts, executes every member, and dumps the tables |

`RoomDatabase` executes real `BEGIN`/`COMMIT`/`ROLLBACK`, so failed `@Transaction` bodies are
observable as rollbacks rather than simulated exceptions. The C++ port keeps the same rule through
`TransactionStack` and the SQLite boundary.

## What is pinned

Fifteen focused scenarios cover all 74 own members plus inherited task and New Chat reads:

* **`01-read-projections`** — navigation/drawer projections, executions, conversation and message
  observation, settings-transfer CRUD, the import-transfer singleton, stuck-message updates, last
  message, execution summaries, conversation/task id lists, both paging shapes, existing-id
  filtering, bulk message lookup, branch-selection CAS, and draft updates.
* **`02-conversation-writes`** — conversation upsert, model/system-prompt/clock updates, title and
  pin CAS, unread toggles, exact model-reference rewrites, run admission, message/run upserts,
  embedding cleanup, conversation-scoped deletes, parent rewrites and cascade deletes.
* **`03-entity-upserts`** — insert-versus-update semantics for every `@Upsert` entity, including
  clearing nullable columns and the import-transfer validation failures.
* **`04-message-subtree`** — recursive subtree deletion, stale-message cleanup, root-run deletion,
  selection rewrites, and every rollback/error branch.
* **`05-create-run`** — run admission with messages, inherited selection updates, both run-read
  shapes (Flow snapshot and suspend snapshot), missing conversation/live-run errors, duplicate IDs,
  and nested transaction rollback.
* **`06-create-conversation-run`** — conversation creation, New Chat draft CAS deletion, settings
  transfer persistence, and rollback when run admission fails.
* **`07-import-graph`** — run-graph import, terminal-run requirements, message reference checks,
  and replace-imported-conversation graph validation/rollback.
* **`08-fork-graph`** — fork graph insertion, embedding clones, message-id remapping, missing
  source/run errors, and a successful embedding-free fork.
* **`09-tool-round`** — tool-round replay and conflict detection, run/pass/current-message checks,
  sequence assignment, and the `inserted` flag.
* **`10-run-terminalization`** — stopping/terminal transitions, in-flight message stops, generation
  finish, stopped-generation batches, and checkpoint rollback.
* **`11-recovery`** — runtime recovery across active, stopping and already-terminal conversations.
* **`12-embeddings`** — explicit and auto-generated ids, message/model uniqueness, lookup order,
  orphan cleanup, and conversation cascade cleanup.
* **`13-bulk-deletes`** — conversation/message bulk deletion, empty-list handling, and the
  cascade behavior of tasks, runs, messages and embeddings.
* **`14-loop-writes`** — inherited loop upsert/deactivate/delete with conversation clock touches.
* **`15-provider-rewrites`** — configured and exact provider/model rewrites across conversations,
  tasks, messages, the New Chat draft and settings JSON, including prefix boundaries.

## Clock masking

Transactions read the wall clock in Kotlin. The driver records every explicit `at`/`nowMs`
parameter, while scenario `"maskClock": true` masks `dataChangedAt` in both the table dump and any
returned `ChatEntity`. The C++ port makes those clock reads explicit parameters so the same trace
is reproducible without freezing the process clock.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1"}],
          "runs": [{"id": "r1", "conversationId": "c1", "status": "COMPLETED",
                    "activeSlot": null, "startedAt": 1}],
          "messages": [{"id": "m1", "conversationId": "c1", "runId": "r1",
                        "text": "hello", "timestamp": 2}]},
 "steps": [{"op": "getMessagesByIds", "ids": ["m1"]}]}
```

Missing optional fields mean NULL. Message `status` defaults to `SUCCESS`, `participant` to
`MODEL`, and `runSequence` to `-1`; run `status` defaults to `ACTIVE` and `activeSlot` to `1`
(an explicit `"activeSlot": null` is preserved). `agora_scenarios chat-dao <stand>` writes the fifteen scenarios
over purpose-built seeds.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`, where a result is an entity, a list,
a row count, a boolean, `null`, or `{error, message}`) followed by `tables` (the full state of
`conversations`, `runs`, `messages`, `embeddings`, `tasks`, `loops`, `new_chat_persist`, and both
settings-transfer tables).

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=kotlinx-serialization-core.jar:kotlinx-serialization-json.jar:kotlinx-coroutines-core.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # regenerate scenarios and verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.

## C++ port

`core/include/agora/core/chat_dao.hpp` and `core/src/chat_dao.cpp` implement the same member set,
publicly inheriting the six ported DAO surfaces. The own `@Transaction` bodies share one
`TransactionStack`; nested calls go through private `_body` helpers so the outermost transaction
owns the real commit/rollback. `core/tests/chat_dao_parity_test.cpp` replays every scenario against
the same schema and compares each step result and the final table dump with these traces.
