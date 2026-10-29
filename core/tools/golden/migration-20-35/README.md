# migration-20-35 golden stand

Parity pin for the SQL-only Room migrations `data/local/migration/Migration20To21.kt` through
`Migration34To35.kt` — fifteen consecutive schema upgrades. All fifteen sources compile
**verbatim from the app tree**. The only substituted surface is the platform side:
`androidx.room.migration.Migration` and `androidx.sqlite.db.SupportSQLiteDatabase` (`platform/`).

The shipped port (`core/src/migration_20_35.cpp`) must reproduce the frozen traces byte for byte
(`core/tests/migration_20_35_parity_test.cpp`).

## What it pins

* every `execSQL` statement, its order and its exact whitespace (`trimIndent` output included);
* the five `TOKEN_USAGE_COLUMNS` in their declared order (20→21);
* the three `maintenance_debt` seed rows and their literal kind names (27→28);
* the two semantic-index tables and the covering index (28→29);
* the `NEEDS_RECONCILE` update and both embedding/ledger column additions (29→30);
* the absence of `IF NOT EXISTS` where the Kotlin source omits it (30→31);
* that the migrations bind no arguments: only SQL text is emitted.

One scenario per source version (15 traces). Negative controls mutate one statement per
migration group and require the parity test to fail.

## Documented boundaries

* Room/SQLite plumbing is platform. The test database records `execSQL` calls instead of running
  SQL against a real schema.
* Only the migrations listed above are covered; `Migration16To17`..`Migration19To20` have their own
  stands.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`core/build/agora_scenarios migration-20-35 core/tools/golden/migration-20-35` rewrites `scenarios/` deterministically (15 cases); rerun
`./run_golden.sh --freeze` afterwards.
