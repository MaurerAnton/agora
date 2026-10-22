# maintenance-debt golden stand

Parity pin for `data/local/MaintenanceDebt.kt`: the debt queue behind every background
maintenance pass — enqueue/claim/complete/release with compare-and-set leases, the orphan
sweeping queries, the run-branch conversation paging, and the run-branch projection repair.

`MaintenanceDebt.kt`, `data/local/RunBranchSelectionIntegrity.kt`, `data/local/RunEntity.kt` and
`model/RuntimeIdentity.kt` compile **verbatim from the app tree**. No extraction is needed: the
debt file carries its own entity.

## The platform boundary

Room's annotation processor is the replaced platform, exactly as in the semantic-index-ledger
stand:

| Path | Purpose |
| --- | --- |
| `platform/MiniRoom.kt` | Reflective executor: reads `@Query` SQL from the RUNTIME-retained stand annotations, binds Kotlin parameter names, implements `@Insert` with Room's ignore-and-renew handshake, maps cursor rows onto entities (including enum columns by `Enum.valueOf` semantics), runs `@Transaction` methods in Room's per-level transaction stack |
| `platform/schema.sql` | The generated DDL: `maintenance_debt` (Migration27To28), `runs` (Migration16To17 — both foreign keys, the unique `(conversationId, activeSlot)` index where SQLite permits multiple nulls), the `embeddings`/`messages` orphan surface, and the documented `conversations` subset |
| `platform/Room.kt`, `platform/RoomDatabase.kt` | Annotation shapes and the `withTransaction` nesting contract |
| `platform/StandChat.kt`, `platform/StandDebtDatabase.kt` | The conversation surface `repairRunBranches` reads, and the database holder for the DAO's own transactions (this file has no ChatDatabase collaborator) |

## Two documented boundaries in the traces

* **`System.currentTimeMillis()`** inside `repairRunBranches` — the value it writes into
  `conversations.dataChangedAt` is masked as `<clock>` when it falls in the wall-clock window,
  and the port takes the timestamp as a parameter. Scenario `11` seeds a
  `dataChangedAt` above any plausible clock, so the `MAX(dataChangedAt + 1, :at)` branch is
  pinned verbatim instead of masked.
* **`ChatEntity`** is the stand-local conversation subset (`id`, `selectedRunBranchesJson`)
  plus the `dataChangedAt` column the compare-and-set query writes.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 13 scenarios, 97 steps |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `MaintenanceDebtTraceDump.kt` | Driver: seeds SQLite, runs the steps, dumps the tables |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Entity validation**: blank kind/identity, unknown state, `revision <= 0`, and the
  claim/claimedAt consistency pair all fail with `IllegalArgumentException("Failed requirement.")`.
* **`enqueue`**: insert-then-renew handshake — an ignored insert (`-1`) must find the row to
  renew, and the ledger must be readable afterwards.
* **`claim`**: candidate order `(updatedAt, kind, identity)`, stale-lease revival without a
  revision bump, the batch limit `1..100`, and the in-transaction re-read checks
  (`"Maintenance debt claim changed inside its transaction"`).
* **`complete` / `release`**: `requireClaim` plus revision/claimId compare-and-set, so a lease
  invalidated by a newer revision (`sql` op bumps it mid-scenario) completes nothing.
* **Orphan sweeping**: `compact_%` ids and missing message rows are both orphans; the id-cursor
  paging and the empty-id list behave like Room's `IN` expansion.
* **`repairRunBranches`**: missing conversation and null JSON return false; malformed JSON
  decodes to null and repairs to `{}`; invalid edges are dropped in input order (including the
  `"null"` key round trip and duplicate-key last-wins-first-position map semantics); an unchanged
  projection never writes (`RunBranchSelectionIntegrity` is the already-ported port).
* **`RunEntity`**: enum columns resolve by constant name (`"No enum constant
  com.newoether.agora.model.RunStatus.BOGUS"`) and the init requires fire on corrupt rows read
  back from the database.

## Scenario format (`scenarios/*.json`)

```json
{"seed": {"conversations": [{"id": "c1", "selectedRunBranchesJson": "{\"r1\":\"r2\"}",
                             "dataChangedAt": 100}]},
 "steps": [{"op": "enqueue", "kind": "RUN_BRANCHES", "identity": "c1", "updatedAt": 5}]}
```

`seed` fills `conversations`, `runs`, `messages` and `embeddings` (runs accept exactly the
column shapes `RunEntity` declares; invalid rows are allowed on purpose so the read path pins
the validation). Steps cover every DAO method; `sql` runs a raw statement (it is also how the
scenario simulates a concurrent revision bump).

## Trace format (`golden/*.trace`)

One JSON document per scenario: `steps` (each `{op, result}`) followed by `tables`, a full dump
of `maintenance_debt`, `conversations`, `runs`, `embeddings` and `messages` in declared column
order, serialized exactly like `Json.encodeToString`.

## Regenerating

```bash
export JAVA_HOME=/path/to/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/path/to/kotlinc KOTLINX_JARS=core.jar:json.jar:coroutines.jar:sqlite-jdbc.jar \
  ./run_golden.sh          # verify
KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate
python3 gen_scenarios.py                                # rewrite scenarios
```

`run_golden.sh` wipes the stand's git-ignored `build/` before each run, so runs are idempotent.
