# migration-17-18 golden stand

Parity pin for `data/local/migration/Migration17To18.kt` — the v17→v18 data-shape repair. The
migration and `RegenerationTreeRepairPlanner.kt` compile **verbatim from the app tree**. The only
substituted surface is the platform side: `androidx.room.migration.Migration`,
`androidx.sqlite.db.SupportSQLiteDatabase` and `android.database.Cursor` are declared under
`platform/` with the shapes the migration uses. `Participant` is lifted by `agora_extract enums-17-18`.

The shipped port (`core/src/migration_17_18.cpp`) must reproduce the frozen traces byte for byte
(`core/tests/migration_17_18_parity_test.cpp`).

## What it pins

* the three cursor projections (`conversations`, `runs`, `messages`) and their bind arguments;
* the complete repair SQL in planner order: `UPDATE runs ... legacyAmbiguous = 1`,
  `UPDATE messages SET parentId`, `DELETE FROM embeddings`, `DELETE FROM messages`,
  `UPDATE messages SET runSequence`, and the final `UPDATE conversations` with both migrated
  selection maps;
* the selection migration (`Map<String?, String>` encoded with the `"null"` key) and the early
  `continue` when no Run is inferred: selections are then left untouched, not normalised;
* anchor selection by `(startedAt, boundary timestamp, runId)`, including input-order reversal and
  equal `startedAt` ties;
* image-count paths: empty images, strict `List<String>` JSON, the literal `"|||"` fallback, and
  their agreement/disagreement through the fingerprint;
* attachment fingerprint sanitization: different `originalUri` values still group (repair), while a
  different `mime` does not;
* unknown participant names falling back to `ERROR`, synthetic (`tool_`/`result_`) user rows
  excluded from boundaries, runs without root output/leading inputs skipped, and three-run groups
  repaired against the earliest anchor.

## Documented boundaries

* Cursor/SQLite plumbing is platform: the port asks `Database` for the same projections instead of
  materialising a real cursor. The SELECT strings are still emitted and pinned by the recording
  shim.
* `readMessages` maps unknown enum names to `ERROR` exactly like `runCatching { valueOf }`.
* Message/conversation ids are ASCII in the app; both UTF-16 and byte ordering agree.
* Fingerprinting (SHA-256, `originalUri` sanitization) is shared with the regeneration-tree port.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```

`python3 gen_scenarios.py` rewrites `scenarios/` deterministically (24 cases); rerun
`./run_golden.sh --freeze` afterwards.
