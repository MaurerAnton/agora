# chat-media-references-dao golden stand

Parity pin for `data/local/ChatMediaReferencesDao.kt`: the five bounded media-reference reads
inherited by the sole `ChatDao` — the attachment sweep over message columns, the tool-payload
scan, the conversation draft sweep, and the singleton New Chat draft read.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatMediaReferencesDao.kt` | the module under test — a plain interface with public members, so **no extraction is needed** |
| `data/local/ChatEntities.kt` | every entity, the four reference projections, `MessageConverters` and `NewChatPersistEntity`; compiles whole, no lifting |
| `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the run row the `messages.runId` foreign key references |
| `model/ChatMessage.kt` and its model dependencies | the enum types behind the converters |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, named-parameter binding, `MessageConverters`' JSON list encoding with the legacy `\|\|\|` fallback |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the database surface |
| `platform/schema.sql` | the Room-generated DDL: all 15 `ChatEntity` columns, all 26 `MessageEntity` columns with both foreign keys and three indices, the `runs` reference table, and the 6-column `new_chat_persist` singleton. `embeddings` is not reachable from this DAO and is omitted. |

## What is pinned

All five members are operations, so no query text goes unpinned.

* **`getMessageAttachmentReferencesPage`** — the two-column eligibility predicate: `images` counts
  only when it is neither `''` nor `'[]'`, `attachmentMeta` only when it is non-null and
  non-empty. The seed covers `images=[]` with a matching meta, `images=''`, `images='[]'`, a
  non-empty meta on an empty image list, and the legacy `\|\|\|` image encoding (which surfaces as
  a two-element list through the app's converter).
* **The optional `pathToken`** — a raw `instr` substring over the stored JSON text, not a LIKE
  pattern: `c` matches `c.png`, `A.PNG` does not match `a.png` (case-sensitive), `[` matches every
  JSON image array, `  ` matches the whitespace meta, and a token found only on an ineligible row
  does not surface it.
* **Keyset pages** — NULL vs empty-string `afterId`, an id before/at/past the last eligible row,
  `LIMIT 0`, and ascending `id` order.
* **`getConversationMessageAttachmentReferencesPage`** — conversation scoping comes first: the
  same predicate yields different pages for `c-1` and `c-2`, an unknown conversation is empty, and
  the conversation id is bound before the two keyset parameters.
* **`getMessageToolMediaReferencesPage`** — `toolCallJson` must be non-null and non-empty (an
  empty string is excluded, whitespace is included), keyset and `LIMIT` same as above.
* **`getConversationDraftAttachmentReferencesPage`** — non-null/non-empty drafts, `instr`
  case sensitivity (`A.png` vs `a.png`), keyset, limit and `LIMIT 0`.
* **`getNewChatDraftAttachmentReference`** — the `id = 0` singleton with a draft, an empty draft,
  a NULL draft and an absent row; a NULL token returns the row, an empty-string token matches every
  non-empty draft, and an unknown token returns null.
* **Read-only**: the trailing table dump re-renders `conversations(id, draftAttachments)`,
  `messages(id, conversationId, images, attachmentMeta, toolCallJson)` and the full
  `new_chat_persist` row for every trace.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c-1"}],
          "runs": [{"id": "run-c-1", "conversationId": "c-1"}],
          "messages": [{"id": "m-01", "conversationId": "c-1", "runId": "run-c-1",
                        "images": ["a.png"]}],
          "newChatPersist": []},
 "steps": [{"op": "getMessageAttachmentReferencesPage", "afterId": null, "limit": 100}]}
```

Message fields default like the stand driver: `text=''`, `images='[]'`, `participant=MODEL`,
`status=SUCCESS`, `timestamp=0`, `runSequence=-1`. `gen_scenarios.py` writes eight scenarios over
purpose-built seeds.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`, where a result is a projection
array, a projection object, `null`, or `{error, message}`) followed by `tables`, serialized exactly
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
