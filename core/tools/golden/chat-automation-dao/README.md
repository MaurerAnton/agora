# chat-automation-dao golden stand

Parity pin for `data/local/ChatAutomationDao.kt`: task and loop persistence inherited by
`ChatDao` — observed task/loop reads, `@Upsert`, model-reference rewrites, clock-change CAS
updates, loop deactivation, and every delete shape.

## Sources that compile verbatim

| Source | Why |
| --- | --- |
| `data/local/ChatAutomationDao.kt` | the module under test — a plain interface, so **no extraction is needed** |
| `data/local/ChatEntities.kt` | `TaskEntity`, `LoopEntity`, the other entities and the converters; compiles whole, no lifting |
| `data/local/RunEntity.kt`, `model/RuntimeIdentity.kt` | the run row referenced by `messages.runId` |
| `model/ChatMessage.kt` and its model dependencies | the enum types behind the converters |

## The platform boundary

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | reflective executor: `@Query` SQL against real SQLite, named-parameter binding, `@Upsert` as insert-then-update-by-primary-key, and cold single-emission Flows |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | annotation shapes and the database surface |
| `platform/schema.sql` | the Room-generated DDL: `conversations`, `runs`, `messages`, and the 12-column `tasks` / 8-column `loops` tables (with the loop → conversation foreign key) |

The observed members (`getAllTasks`, `getLoop`, `observeActiveLoops`) return Room Flows. The stand
has no invalidation bus, so each Flow executes the declared SELECT when collected and emits the
current rows once; the C++ port exposes that initial emission, and re-emission belongs to the
caller's store.

## What is pinned

All twenty members are operations, so no SQL goes unpinned.

* **Task reads** — `getAllTasks` (`ORDER BY createdAt DESC`), `getAllTasksList` (insertion order),
  `getTask` (present/absent), `getEnabledTasks` (only `enabled = 1`, insertion order).
* **`upsertTask`** — insert when absent and update-by-primary-key when present, overwriting every
  non-key column (including clearing `systemPromptId`/`runAt`/`lastRunAt` to NULL).
* **Model references** — exact replacement (`openai:gpt-4` must not touch `openai:gpt-4o`) and the
  provider-prefix rewrite (`openai` → `azure`), including the prefix boundary: `openai` without a
  colon and `openaix:foo` are untouched. A NULL replacement clears the column.
* **Task clock CAS** — `updateTaskNextRunAtIfUnchanged` succeeds once, then fails on the stale
  expected time, the wrong cron, the wrong id, and a disabled task.
* **Loop reads** — `getLoop` (present/absent), `getActiveLoops`, `observeActiveLoops`,
  `getAllLoopsList` and `getLoopsForConversationSnapshot`.
* **`upsertLoopRow`** — insert, update, and clearing `prompt`/`maxCycles` to NULL; an updated loop
  becomes visible to `observeActiveLoops`.
* **Loop clock CAS** — `updateLoopNextFireAtIfUnchanged` succeeds once, then fails on a stale fire
  time, a wrong revision, and an inactive loop.
* **`deactivateLoopRowIfUnchanged`** — sets `active = 0`, `nextFireAt = 0`, bumps `revision`, and
  normalizes `maxCycles`; a repeat CAS fails.
* **Deletes** — `deleteTask` (Unit), `deleteAllTasks`, `deleteLoopRow` (row count 1/0), and
  `deleteAllLoops`.
* **Mutation visibility**: every trace ends with a dump of the full `tasks` and `loops` tables.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1"}],
          "tasks": [{"id": "t1", "name": "t1", "prompt": "p", "cronExpr": "0 0 * * *",
                     "nextRunAt": 100, "enabled": true, "createdAt": 1}],
          "loops": [{"conversationId": "c1", "intervalMs": 60000, "nextFireAt": 5000,
                     "cycleCount": 0, "active": true, "revision": 0}]},
 "steps": [{"op": "getAllTasks"}]}
```

Missing optional fields mean NULL; `name`/`prompt`/`cronExpr` default to `''`, `enabled`/`active`
to true, `nextRunAt`/`nextFireAt`/`createdAt`/`revision` to 0. `agora_scenarios chat-automation-dao`
writes six scenarios over purpose-built seeds.

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`, where a result is an entity, a list,
a row count, `null`, or `{error, message}`) followed by `tables`, serialized exactly like
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
