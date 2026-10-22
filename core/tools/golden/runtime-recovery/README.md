# runtime-recovery golden stand

Parity pin for `data/local/ChatDaoRuntimeRecovery.kt`: the `@Transaction` body of
`ChatDao.recoverConversationRuntime` — one pass that repairs a conversation's runtime after a
process death by folding the live Run back through the runtime reducer, rewriting the in-flight
message checkpoints, validating the durable run-branch projection, stopping stuck rows and
touching the export watermark exactly once.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatDaoRuntimeRecovery.kt` | the module under test (the driver shares its module, so `internal` needs no widening) |
| `data/local/RunBranchSelectionIntegrity.kt`, `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the projection validator and the run entity it reads |
| `model/ConversationRuntimeReducer.kt` + the Kotlin cluster (`ConversationCommand`, `RunState`, `RunEffect`, `RuntimeTransition`) | the reducer the body drives |
| `model/RunRecoveryPolicy.kt` | `recoverMessageStatus` / `stopIncompleteToolsJson` |
| `model/ChatMessage.kt` and its model dependencies | `MessageStatus`, `Participant`, `MessageSegment`, `ToolExecutionStates` |
| `extract_recovery_sources.py` output | `MessageEntity`, `MessageStreamCheckpoint`, `encodeSelectionMap` lifted byte for byte from `ChatEntities.kt` / `ChatDao.kt` |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/ChatDao.kt` | the nine ChatDao queries the body calls, transcribed with the app's own query text (ChatDao.kt itself is a 795-line interface belonging to another module) |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, `@Update(entity = ...)` projection updates by primary key, enum columns by `Enum.valueOf`, and `MessageConverters`' `List<String>` JSON encoding incl. the legacy `\|\|\|` fallback |
| `platform/schema.sql` | the Room-generated DDL: all 26 `MessageEntity` columns with both foreign keys and three indices, the Migration16To17 `runs` table, and the documented `conversations` subset |
| `platform/DebugLog.kt` | the Android logging sink dropped — `recoveryStage` is timing instrumentation with no decision in it (documented) |
| `platform/StandChat.kt`, `platform/StandChatDatabase.kt` | the conversation surface the body reads and the database holder for the `@Transaction` wrapper |

`at` (the `dataChangedAt` bump) is a scenario parameter and `System.currentTimeMillis()` feeds
only the logging stub, so **every trace here is deterministic without clock masking**.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 9 scenarios, 15 steps |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `RuntimeRecoveryTraceDump.kt` | Driver: seeds SQLite, runs `recoverConversationRuntime`, dumps the tables |
| `extract_recovery_sources.py` | Lifts `MessageEntity`, `MessageStreamCheckpoint`, `encodeSelectionMap` and the recovery body |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Missing/quiet conversations** return 0 rows and must not touch the watermark; a
  result-returning `sql` step pins the platform failure text (`Query returns results`).
* **Live-run recovery**: `RecoverDurableRun` → message checkpoints (`MODEL + in-flight → STOPPED`,
  incomplete `toolState → "stopped"` inside the rewritten JSON) → `terminalizeLiveRun`
  (`STOPPED`/`PROCESS_RECOVERED`, slot cleared, timestamps = `at`) → `RecoveryCompleted` accepted,
  and `+1` for the durable terminalization itself.
* **StopStuck sweep is conversation-wide**: user rows in `SENDING` are stopped there (the message
  policy never touches user rows), rows already stopped by the checkpoint pass are not counted
  twice, and messages of terminal runs are only reachable through the sweep.
* **Branch projection**: invalid edges are dropped and written back (`compareAndSetRunBranchSelections`
  without a watermark bump); valid projections never write; **malformed JSON is skipped entirely**
  (unlike maintenance's repair, which rewrites it to `{}`); null projections skip.
* **Watermark**: `MAX(dataChangedAt + 1, :at)` — a `sql` corruption step plus a below-watermark
  `at` pins the forward-only move, and a no-change conversation keeps its value.
* **Converter round trip**: legacy `a|||b` images decode for the checkpoint and re-encode as a
  JSON array; unchanged rows keep their stored text verbatim; empty text becomes `[]` only when
  the row is rewritten.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1", "selectedRunBranchesJson": null, "dataChangedAt": 100}],
          "runs": [{"id": "r1", "conversationId": "c1", "status": "ACTIVE", "activeSlot": 1,
                    "startedAt": 1000, "lastCheckpointAt": 1000, "currentPass": 0,
                    "legacyAmbiguous": false}],
          "messages": [{"id": "m1", "conversationId": "c1", "text": "body",
                        "status": "THINKING", "participant": "MODEL", "timestamp": 10,
                        "runId": "r1", "runSequence": 0, "tokenCount": 0}]},
 "steps": [{"op": "executeRuntimeRecovery", "conversationId": "c1", "at": 5000}]}
```

`seed` fills `conversations`, `runs`, `messages` (valid entity rows by construction; `imagesRaw`
seeds a stored text verbatim). Steps are `executeRuntimeRecovery` and `sql`.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}` — changed-row counts, or
`{"error": class, "message": ...}`) followed by `tables`, a full dump of `conversations`, `runs`
and `messages` in declared column order, serialized exactly like `Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar:coroutines.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate
python3 gen_scenarios.py                                # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
