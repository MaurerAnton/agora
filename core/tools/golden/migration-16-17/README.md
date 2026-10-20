# migration-16-17 golden stand

Parity pin for `data/local/migration/Migration16To17.kt` — the SQL half of the v16→v17 upgrade.
The migration and `LegacyRunBackfillPlanner.kt` compile **verbatim from the app tree**. The only
substituted surface is the platform side: `androidx.room.migration.Migration` and
`androidx.sqlite.db.SupportSQLiteDatabase/Cursor` are declared under `platform/` with the shapes the
migration uses. The model enums are lifted declaration-for-declaration by `extract_enums.py`.

The shipped port (`core/src/migration_16_17.cpp`) must reproduce the frozen traces byte for byte
(`core/tests/migration_16_17_parity_test.cpp`).

## What it pins

* the complete SQL surface in execution order: `ALTER TABLE`, `CREATE TABLE/INDEX`, `INSERT` into
  `runs`, `UPDATE messages`, the selection update, the `messages_new` copy and its indexes;
* every bind argument with its Kotlin runtime type (`String`/`Long`/`Int`/null), which pins the
  `activeSlot`/`stopRequestedAt` NULL literals, `currentPass = 0`, the boolean-as-integer
  `legacyAmbiguous` and the `consumedAtPass` nullability;
* the planned Run ids and the `selectedRunBranchesJson` payload (`Map<String?, String>` encoded
  with the `"null"` root key);
* conversation iteration order, per-conversation message projections, and the final
  `SELECT COUNT(*)` guard;
* failure paths: a blank conversation id aborts with the planner's `IllegalArgumentException`
  after the recorded prefix, and messages that no conversation query visited abort with
  `IllegalStateException: v16-to-v17 migration left N messages without a Run`;
* selection decoding: valid JSON, invalid JSON, blank text and non-string values (all failures
  fall back to an empty selection map, matching `decodeSelections`).

## Documented boundaries

* Cursor/SQLite plumbing is platform: the port asks `Database` for the same projections instead of
  materialising a real cursor. The SELECT strings are still emitted and pinned by the recording
  shim.
* `readLegacyMessages` maps unknown enum names to `ERROR` exactly like `runCatching { valueOf }`.
* Message ids and conversation ids are ASCII in the app; both UTF-16 and byte ordering agree.
* `timestamp` is a Kotlin `Long`; the port carries `long long`.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (18 cases); rerun
`./run_golden.sh --freeze` afterwards.
