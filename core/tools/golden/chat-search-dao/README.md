# chat-search-dao golden stand

Parity pin for `data/local/ChatSearchDao.kt`: the search-eligibility and bounded-projection
surface inherited by the sole `ChatDao`. All fourteen members are read-only `@Query` reads.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatSearchDao.kt` | the module under test — a plain interface with public members, so **no extraction is needed** |
| `data/local/ChatEntities.kt` | all four entities plus `MessageConverters` and `IndexableMessage`; compiles whole, no lifting |
| `data/local/EmbeddingSearchRow.kt` | the semantic-scoring projection |
| `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the run row the `messages.runId` foreign key references |
| `model/ChatMessage.kt` and its model dependencies | `MessageStatus`, `Participant`, the message model cluster |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, named-parameter binding, collection expansion exactly like Room's `appendPlaceholders` (empty list → `IN (NULL)`), enum columns by `Enum.valueOf`, `MessageConverters`' JSON list encoding with the legacy `\|\|\|` fallback |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes (including `@TypeConverter`/`@ColumnInfo`) and the database surface |
| `platform/schema.sql` | the Room-generated DDL: all 15 `ChatEntity` columns with the `taskId` index, all 26 `MessageEntity` columns with both foreign keys and three indices, the `runs` reference table, and the 7-column `embeddings` table with its two indices |

## What is pinned

Every member of the interface is an operation, so no query text goes unpinned. The dataset
targets the predicates that differ between the queries and between SQLite's two id filters:

* **`searchMessages`** — text *or* conversation-title matches (title matches return every eligible
  message of the conversation), ASCII case folding, the caller-supplied `ESCAPE '\'` (scenario 02
  pins `100\%` vs the unescaped `%` wildcard, `a\_b` vs the `_` wildcard, and a literal
  backslash), `participant IN ('USER','MODEL')`, `text != ''`, and the exact `substr` prefixes
  (`tool_`/`result_`/`compact_`): `resultx`, `toolx` and the seven-character `compact` survive the
  substr filter, while the embedding filters' `NOT LIKE 'result_%'` / `'tool_%'` / `'compact_%'`
  exclude them because `_` is a wildcard there.
* **`getMessagesWithCitationSegmentsPage`** — the exact `'"type":"citation"'` substring (a spaced
  `"type": "citation"` is *not* a match), MODEL-only, and only the `tool_`/`result_` prefixes are
  filtered here — a `compact_` message with citation JSON is returned. Keyset pages start at
  `""`, at an existing id, at a non-matching id, and past the end.
* **`getEmbeddingSearchPage`** — model filter, `e.id > afterId`, task exclusion (including an
  empty-string `taskId`, which is not NULL), participant filter, unicode `LENGTH` (`éé` and `漢字`
  are 2, a single `é` and an emoji are 1), the `_`-wildcard NOT LIKE prefixes, a message with no
  message row, and `LIMIT` order/edge (0 and past-the-end).
* **Counts** — `getEmbeddingCountByModel` (including a model with zero eligible rows),
  `getEmbeddingCountsByModels` (two-model and reversed IN lists, an empty list, an unknown model),
  and `getIndexableMessageCount`.
* **`getSearchableMessagesPage` / `getUnembeddedMessagesPage`** — NULL vs empty-string `afterId`,
  exact keyset boundaries, `LIMIT 0`, and the per-model `NOT EXISTS` (a message embedded for
  `model-b` is still unembedded for `model-a`).
* **`getSearchableMessagesByIds`** — duplicates collapse, unknown ids, an empty list, and every
  exclusion class in one request. The scenario seed inserts rows in ascending id order, so the
  query's absent ORDER BY cannot depend on a plan (index order and rowid order coincide).
* **`isMessageSearchable`** — one true row plus every false class (task conversation, ERROR
  participant, empty text, each excluded prefix, unknown id).
* **Conversation surface** — `getSearchableConversation` full-row mapping (all 15 columns,
  including booleans and nulls), task/unknown ids return null, the count, and both offset pages
  with tie-breaking on equal `lastUpdated`, offsets past the end, and `LIMIT 0`.
* **Read-only**: the trailing table dump re-renders all four tables; every trace proves the
  queries leave the store byte-identical.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1", "title": "Alpha Notes", "lastUpdated": 300}],
          "runs": [{"id": "r1", "conversationId": "c1", "status": "COMPLETED",
                    "activeSlot": null, "startedAt": 100, "lastCheckpointAt": 100,
                    "endedAt": 200, "endReason": "MODEL_COMPLETED", "currentPass": 0,
                    "legacyAmbiguous": false}],
          "messages": [{"id": "m1", "conversationId": "c1", "text": "alpha",
                        "participant": "MODEL", "timestamp": 100, "runId": "r1"}],
          "embeddings": [{"id": 1, "messageId": "m1", "modelId": "model-a",
                          "embedding": "0a0b", "chunkText": "chunk", "dimension": 2}]},
 "steps": [{"op": "searchMessages", "query": "alpha", "limit": 10}]}
```

`seed` fills the four tables with valid rows (conversation/message fields default the way the
entities do). `agora_scenarios chat-search-dao <stand>` writes nine scenarios over one shared dataset.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}` — entity/projection renders, a
scalar, or `{"error": class, "message": ...}`) followed by `tables`, a full dump of
`conversations`, `runs`, `messages` and `embeddings` in declared column order, serialized exactly
like `Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=kotlinx-serialization-core.jar:kotlinx-serialization-json.jar:kotlinx-coroutines-core.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # regenerate scenarios and verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
