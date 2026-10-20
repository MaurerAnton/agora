# migration-19-20 golden stand

Parity pin for `data/local/migration/Migration19To20.kt` — the SQL-only repair for message rows
that were already oversized before `MessagePersistenceGuard` existed.

The migration and `util/Constants.kt` compile **verbatim from the app tree**. Only the platform
side is substituted: `androidx.room.migration.Migration` and
`androidx.sqlite.db.SupportSQLiteDatabase` (`platform/`). `Migration19To20.kt` names exactly one
symbol from `MessagePersistenceGuard` (`TRUNCATION_MARKER`); compiling that whole object would drag
in `ChatMessage.kt` (Compose, Markdown, provider models), so `extract_marker.py` lifts the real
constant line into `extracted/` on every run. The generated file is never edited by hand.

The shipped port (`core/src/migration_19_20.cpp`) must reproduce the frozen trace byte for byte
(`core/tests/migration_19_20_parity_test.cpp`).

## What it pins

* the exact three `UPDATE messages` statements, their order and their whitespace;
* the Kotlin runtime type of every bind argument: `Int` limits stay `i:`, the truncation marker
  stays `s:` (a `Long` or a different marker changes the trace);
* the byte-length guard on `toolCallJson` (`length(CAST(... AS BLOB))`), the text guard on `text`
  and the null guard on `thoughts`;
* the absence of any cursor read: the migration is pure `execSQL`.

The migration has no inputs, so one scenario exercises the complete statement list. Negative
controls mutate the port (drop a statement, reorder, change a limit or the marker, swap an `Int`
for a `Long`) and require the parity test to fail.

## Documented boundaries

* Room/SQLite plumbing is platform. The test database records `execSQL` calls instead of running
  SQL.
* The truncation marker is extracted verbatim from `MessagePersistenceGuard.kt`; its value and
  bytes are pinned by the trace and by `Constants`/guard unit tests in the app.
* Bind values are constants from `Constants.kt`; any drift in those constants changes the trace.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=...core.jar:...json.jar ./run_golden.sh           # verify
KOTLINX_JARS=... ./run_golden.sh --freeze                      # refreeze traces
```
