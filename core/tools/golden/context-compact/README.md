# context-compact golden stand

Parity pin for `data/local/ChatContextCompactDao.kt`: the atomic Context-Compact graph contract
that ChatDao inherits — beginning a Recompact (swapping a terminal Compact's dedicated Run for a
fresh active one while the message keeps its identity and graph position) and removing a Compact
(reparenting its children, dropping the row and its embeddings, and collapsing both branch
projections).

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatContextCompactDao.kt` | the module under test — a plain interface rather than a `@Dao`, every member public, so **no extraction is needed for it** |
| `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the run row both transactions validate against |
| `model/ChatMessage.kt` and its model dependencies | `MessageStatus`, `Participant` |
| `util/Constants.kt` | `COMPACT_MSG_PREFIX` |
| ``agora_extract message-entity`` output | `MessageEntity` lifted byte for byte from `ChatEntities.kt` |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, `@Insert` (ABORT), enum columns by `Enum.valueOf`, `MessageConverters`' JSON list encoding with the legacy `\|\|\|` fallback, `Unit`-returning members coercing the row count the way Room's generated code does |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the `withTransaction` nesting contract |
| `platform/schema.sql` | the Room-generated DDL: all 26 `MessageEntity` columns with both foreign keys and three indices, the Migration16To17 `runs` table, the `embeddings` orphan surface, and the documented `conversations` subset plus `lastUpdated`/`dataChangedAt` |
| `platform/StandChat.kt`, `platform/StandChatDatabase.kt` | the conversation surface the transactions read, and the database holder for the two `@Transaction` bodies |

## Two documented boundaries in the traces

* **`System.currentTimeMillis()`** inside `removeContextCompact` — the `lastUpdated`/`dataChangedAt`
  values it writes are masked as `<clock>` when they fall in the wall-clock window, and the port
  takes the timestamp as a parameter. Scenario-supplied `at` values (and the Recompact's
  `replacementRun.startedAt`) sit outside the window and print verbatim.
* **`PRAGMA foreign_keys`** — SQLite's foreign keys would otherwise make three guards structurally
  unreachable (a conversation or run cannot disappear without cascading the message that reads it).
  Scenario `05` turns enforcement off for the deletions so the `?: return false` paths and the
  missing-conversation error are pinned rather than assumed.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 14 scenarios, 67 steps |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `ContextCompactTraceDump.kt` | Driver: seeds SQLite, runs every interface member, dumps the tables |
| ``agora_extract message-entity`` | Lifts `MessageEntity` out of `ChatEntities.kt` |
| `agora_scenarios context-compact <stand>` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

Every one of the 21 interface members is an operation, so no query goes unpinned.

## What is pinned

* **Recompact success**: the full surgery — fresh Run inserted, the Compact message re-pointed
  (`runSequence 0`, empty text, `SENDING`, new model), run children repointed, the run projection
  rewritten (a selected child of the old Run is carried to the new one), the old Run deleted, and
  the watermark moved to `replacementRun.startedAt`.
* **Every Recompact guard**, with its exact JVM class and message: `Compact disappeared`,
  `Conversation became busy during Recompact`, `Compact Run disappeared`, `Compact Run is not
  terminal`, `Compact Run is not independently replaceable`, `Compact message is not terminal`,
  `Conversation <id> disappeared`, `Selected message branch changed before Recompact`,
  `Compact parent disappeared`, `Recompact requires a fresh Run`, `Compact Run ancestry changed`,
  `Selected Run graph is inconsistent`, plus the message-less `require`s (`Failed requirement.`).
* **Branch projection equality is order independent** — scenario 12 passes an expected JSON whose
  keys are ordered differently from the stored one, which Kotlin's `Map.equals` accepts and the
  Recompact therefore completes, writing the stored string back verbatim.
* **Remove**: both promotion branches at both levels (a suffix child is carried to the parent key;
  without one the parent key is dropped), the shared-Run case where the Run and its projection
  survive, message reparenting, embedding deletion, and the clock watermark.
* **Malformed/null projections decode to an empty map** (unlike the maintenance repair, which
  skips malformed input) — they are rewritten as `"{}"`, including over a NULL column.
* **The write half of the interface**: every update/delete reports its row count, the four
  selection updaters move `lastUpdated`/`dataChangedAt` independently, `replaceContextCompactMessageRun`
  both matches (1) and refuses (0), and `nextRunSequence` distinguishes `MAX(seq)+1` from 0.
* **`RunEntity` validation on reads**: the four `init` rules and `Enum.valueOf` fire for corrupt
  rows, through `getRun` and `getLiveRun` alike.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1", "selectedBranchesJson": "{\"m1\":\"compact_1\"}"}],
          "runs": [{"id": "r1", "conversationId": "c1", "status": "COMPLETED",
                    "activeSlot": null, "startedAt": 10, "lastCheckpointAt": 10,
                    "endedAt": 20, "endReason": "MODEL_COMPLETED", "currentPass": 0,
                    "legacyAmbiguous": false}],
          "messages": [{"id": "compact_1", "conversationId": "c1", "parentId": "m1",
                        "text": "summary", "status": "SUCCESS", "participant": "MODEL",
                        "timestamp": 45, "runId": "r1", "runSequence": 0}]},
 "steps": [{"op": "removeContextCompact", "messageId": "compact_1"}]}
```

`seed` fills `conversations`, `runs`, `messages` (valid entity rows by construction; `imagesRaw`
seeds a stored text verbatim) and `embeddings`. Steps are the interface members plus `sql`, which
also carries the `PRAGMA` statements scenario 05 needs.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}` — entity renders, row counts, or
`{"error": class, "message": ...}`) followed by `tables`, a full dump of `conversations`, `runs`,
`messages` and `embeddings` in declared column order, serialized exactly like
`Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar:coroutines.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate
../../../build/agora_scenarios context-compact .   # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
