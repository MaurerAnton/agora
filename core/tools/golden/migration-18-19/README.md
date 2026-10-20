# migration-18-19 golden stand

Parity pin for `data/local/migration/Migration18To19.kt` — the forward repair for devices that
opened the rejected first v18 build. The migration, `V18RegenerationEdgeRepairPlanner.kt` and
`util/Constants.kt` compile **verbatim from the app tree**. The only substituted surface is the
platform side: `androidx.room.migration.Migration`, `androidx.sqlite.db.SupportSQLiteDatabase` and
`android.database.Cursor` (`platform/`). `Participant` is lifted by `extract_enums.py`.

The shipped port (`core/src/migration_18_19.cpp`) must reproduce the frozen traces byte for byte
(`core/tests/migration_18_19_parity_test.cpp`).

## What it pins

* both projections (`runs` with `legacyAmbiguous`, `messages` with participant/run fields) and the
  absence of bind arguments;
* the `legacyAmbiguous` integer gate including non-0/1 values, the parent-run existence check and
  the null-parent skip;
* canonical user selection by `(runSequence, id)`, the repaired-root requirement, and exclusion of
  synthetic (`tool_`/`result_`) rows;
* dangling-edge ordering by `(runSequence, id)`, the duplicate-id first-position/last-parent map
  semantics, and cross-run update order;
* the exact ordered `UPDATE messages SET parentId = ? WHERE id = ?` statements.

## Documented boundaries

* Cursor/SQLite plumbing is platform: the port asks `Database` for the same projections instead of
  materialising a real cursor. The SELECT strings are still emitted and pinned by the recording
  shim.
* Unknown participant names map to `ERROR` exactly like `runCatching { valueOf }`.
* Message/run ids are ASCII in the app; both UTF-16 and byte ordering agree.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (21 cases); rerun
`./run_golden.sh --freeze` afterwards.
