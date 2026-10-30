# ConversationExportSnapshotReader golden stand

This stand pins `app/src/main/java/com/newoether/agora/data/ConversationExportSnapshotReader.kt`
against a real SQLite database. The reader source and every entity/model file it references are
compiled verbatim; only the Android and Room platform surface around it is substituted.

## What the reader does

`readSnapshot(includeBody, onRecord)` opens a second database connection, starts one deferred
reader transaction, and emits:

1. every conversation header from `getAllConversationsList()`;
2. for each conversation accepted by `includeBody`: its snapshot runs, then id-ordered message
   pages of 64, then its loop row;
3. all task rows, after every conversation body.

The transaction commits only when the whole traversal succeeds. A thrown `includeBody`,
`onRecord`, or cancellation rolls it back; the reader then closes the database and shuts down its
executor in `finally`.

## Stand boundary

| App source | Role in this stand |
| --- | --- |
| `data/ConversationExportSnapshotReader.kt` | Compiled verbatim. |
| `data/local/ChatDao.kt`, its six parents, `ChatDaoRuntimeRecovery.kt`, entities and model deps | Compiled verbatim; MiniRoom executes their declared SQL over schema 36. |
| `data/local/ChatDatabase.kt` | Replaced by `platform/StandChatDatabase.kt`: the Android Room composition root and migration chain are a platform boundary, but the stand opens the same schema-36 SQLite file and exposes `chatDao()`, a reader connection, and `close()`. |
| `androidx.room.useReaderConnection` / `Transactor` | Replaced by `platform/RoomReader.kt`; `withTransaction` runs a real JDBC commit/rollback. |
| `android.content.Context`, `android.os.Process` | Replaced by `platform/Android.kt` / `platform/AndroidProcess.kt`; only the database path and a thread-priority hint. |

`platform/MiniRoom.kt` is the Room annotation-processor substitute shared with the `chat-dao`
stand. It contains no reader logic: all SQL, paging, callback order, and task-tail ordering come
from the compiled app sources.

## Scenarios

`agora_scenarios conversation-export-snapshot-reader` writes 10 scenarios / 13 reader steps:

- empty database;
- one complete conversation with runs, messages, a loop, and tasks;
- multiple conversations with per-conversation body skipping;
- exactly 64 messages and 65 messages, pinning the page boundary and the `afterId` handoff;
- every body skipped, leaving headers plus the task tail;
- `includeBody` failure, then a successful retry;
- `onRecord` failure, then a successful retry;
- lexical message-id order, equal-`startedAt` run ties, and task insertion order;
- no-body conversations with multiple tasks.

Each step records `records` and `includeBodyCalls`, plus `error`/`message` when a callback throws.
The trace ends with a primary-key inventory for every table, proving the traversal is read-only.

## C++ port

`core/include/agora/core/conversation_export_snapshot_reader.hpp` exposes `SnapshotStore` with
the same five DAO reads and the three transaction calls, plus the concrete
`ChatDaoSnapshotStore` adapter. `read_snapshot` mirrors the Kotlin order and rollback path;
coroutine cancellation is exposed as an optional `ensure_active` callback at the exact Kotlin
call sites. The caller owns the `SqlExecutor`/connection; the adapter brackets the traversal in
the same transaction contract.

`core/tests/chat_dao_parity_test.cpp` contains the
`conversation_export_snapshot_reader_golden_parity` test. It seeds the same schema-36 database with
the scenario SQL, drives the C++ reader through the core `ChatDaoSnapshotStore`, and compares
every step and table-key inventory with the frozen traces.

## Commands

```sh
JAVA_HOME=... KOTLINC=... KOTLINX_JARS=... ./run_golden.sh
JAVA_HOME=... KOTLINC=... KOTLINX_JARS=... ./run_golden.sh --freeze
```

The Kotlin driver uses a temporary file-backed SQLite database, exactly as the production reader
opens a durable database. The C++ test uses the same DDL in memory; the transaction contract and
all SQL are identical.
