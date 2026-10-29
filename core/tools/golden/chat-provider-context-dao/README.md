# chat-provider-context-dao golden stand

Parity pin for `data/local/ChatProviderContextDao.kt`: the payload-free context reads inherited by
`ChatDao` — the nullable provider selection state, the timestamp-ordered message topology, its
observed Flow, and the transactional state+topology snapshot.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatProviderContextDao.kt` | the module under test — a plain interface with one `@Transaction` default-method body, so **no extraction is needed** |
| `data/local/MessageContextTopology.kt` | `MessageContextTopology`, `ConversationProviderContextState`, `ProviderContextTopologySnapshot` and the execution summary row |
| `model/ChatMessage.kt` and the model cluster (`AttachmentMeta`, `MessageSource`, `TokenUsage`, `ModelId`, `ContextBudget`, `Citation`, `MessagePersistenceGuard`, `RuntimeIdentity`, `Constants`) | the `MessageStatus`/`Participant` enum types behind the projection |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, named-parameter binding, `@Transaction` nesting, and a cold single-emission Flow for observed queries |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the transaction surface |
| `platform/schema.sql` | the Room-generated DDL: the full 15-column `conversations`, 26-column `messages` with both foreign keys and three indices, and the `runs` reference table. `new_chat_persist` and `embeddings` are not reachable from this DAO and are omitted. |

`observeMessageContextTopology` returns `Flow<List<MessageContextTopology>>`. Room's Flow
re-runs the query on every invalidation; the stand has no invalidation bus, so its Flow is a cold
flow that executes the declared SELECT when collected and emits the current rows once. That
initial emission is the observable part of the DAO contract — the port exposes the same rows as a
snapshot, and the caller's store owns re-emission.

## What is pinned

All four members are operations, so no query text goes unpinned.

* **`getProviderContextState`** — a present conversation with a JSON selection, a NULL selection,
  `''`, `[]`, and an absent conversation (`null`, not a state with a null field).
* **`getMessageContextTopology`** — `ORDER BY timestamp ASC, id ASC`, not id order: a timestamp
  tie is broken by id, and the ids in the seed are deliberately out of numeric order. All seven
  `MessageStatus` constants and all three `Participant` constants appear in the projection.
  Optional `parentId`, `modelName` and `consumedAtPass` round-trip; `tokenCount` carries non-zero
  values; an unknown conversation and `''` are empty lists.
* **`observeMessageContextTopology`** — the Flow's initial emission has the same rows and ordering
  as the suspend member; an empty conversation emits an empty list.
* **`getProviderContextTopologySnapshot`** — the `@Transaction` body reads the state first, returns
  `null` for an absent conversation without reading messages, and otherwise returns the state
  (including NULL/`''`/`[]`) with the ordered messages. A conversation with no messages returns an
  empty list, not null.
* **Enum failures** — an unknown stored `status` or `participant` surfaces Room's exact
  `IllegalArgumentException` (`No enum constant com.newoether.agora.model.<Enum>.<value>`) from
  both the list member and the transaction member.
* **Read-only**: the trailing table dump re-renders `conversations(id, selectedBranchesJson)` and
  `messages(id, conversationId, parentId, status, participant, timestamp, tokenCount, modelName,
  runId, runSequence, consumedAtPass)` for every trace.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c-1", "selectedBranchesJson": "{\"m-1\":\"m-2\"}"}],
          "runs": [{"id": "run-c-1", "conversationId": "c-1"}],
          "messages": [{"id": "m-01", "conversationId": "c-1", "runId": "run-c-1",
                        "timestamp": 10, "status": "THINKING", "participant": "USER"}]},
 "steps": [{"op": "getProviderContextTopologySnapshot", "conversationId": "c-1"}]}
```

Message fields default like the stand driver: `text=''`, `images='[]'`, `status=SUCCESS`,
`participant=MODEL`, `timestamp=0`, `tokenCount=0`, `runSequence=-1`. `agora_scenarios chat-provider-context-dao` writes
five scenarios over purpose-built seeds.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`, where a result is a projection, a
list, `null`, or `{error, message}`) followed by `tables`, serialized exactly like
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
