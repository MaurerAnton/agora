# native-conversation-graph-importer

Golden stand for `data/NativeConversationGraphImporter.kt` — the three public phases (graph
counts, header read, transactional import) over the chained conversation graph.

## What compiles verbatim

The importer plus everything it touches, assembled from the existing stands' recipes:

- `NativeConversationGraphImporter.kt`, `NativeConversationGraphSource.kt`,
  `NativeGraphJsonValue.kt`, `NativeConversationMediaRestorer.kt`, `NativeBackupArchive.kt`,
  `NativeRunArchivePolicy.kt`, `MessageSource.kt`, `MaintenanceDebt.kt`,
  `SemanticIndexLedger.kt` (with `EmbeddingEntity` from the real `ChatEntities.kt` — unlike the
  ledger stand, no extraction is needed), both migration planners, the ChatDao/entity cluster and
  the settings/format models;
- `sanitizeImportedTask`/`sanitizeImportedLoop`/`sanitizeImportedConversation` and the nested
  `DataImporter.ImportStrategy` enum are lifted byte-for-byte from `DataImporter.kt` by
  `agora_extract import-pieces` (the full class drags in the restore composition root this module
  never touches).

## Platform substitutes

1. **android.util.Json\*** — reused from the `native-conversation-graph-source` stand: this
   stand's `run_golden.sh` stages that stand's vendored AOSP sources (one documented edit:
   ravenwood annotation lines dropped) and compiles them with `javac`.
2. **Android/Room surface** — one merged `Context`/`ContentResolver` (the archive's descriptor
   path plus the URI stream surface), a `FileProvider` stub (only reachable from
   `restoreConversationMedia`, which the scenarios never run), a `MaintenanceDebtWorker.schedule`
   stub for the constructor default, and the stand `ChatDatabase` over the Room-generated schema
   36 database: `chatDao()`/`semanticIndexDao()`/`maintenanceDebtDao()` MiniRoom proxies (the
   schema already carries the `embeddings`, `semantic_index_*` and `maintenance_debt` tables)
   plus Room's `BEGIN`/`COMMIT`/`ROLLBACK` stack, which the import's
   `withSemanticGraphMutation` transaction runs on.
3. **The injected call-site values** — the driver constructs `Json { ignoreUnknownKeys = true;
   coerceInputValues = true }` (DataImporter's config), a `resolveSystemPromptId` lambda over the
   scenario's map (`containsKey` semantics, null-preserving), a recording `scheduleMaintenance`
   and a scenario-seeded `RestoredMedia` (so the FileProvider/archive paths never run).

## Trace surface

- `count` → `ConversationGraphCounts` from the chained v5 stream (or legacy `conversations.json`).
- `headers` → every field of `ConversationGraphHeaders`: task/conversation/loop sanitization,
  prompt and task-id resolution, dangling-task clearing under REPLACE (vs MERGE id retention),
  parent-before-child run ordering and the source-uniqueness flag, draft attachment restore, and
  the conversation-settings extraction (kotlinx `encodeDefaults = false` shape).
- `import` → the returned transfer id (masked `<uuid:N>`), the `scheduleMaintenance` flag, and a
  table dump after the transaction: conversations/runs/messages/tasks/loops, embeddings (the
  mutation's clears), the settings import transfer, `maintenance_debt` and
  `semantic_index_ledger`. This pins MERGE upsert-vs-skip rules, the `getRun` insert guard, both
  run-planning branches (legacy backfill with planner-made run ids, and the repair branch with
  inferred parents, deleted duplicate inputs, reparenting and resequencing), the recovered-run
  status rewrite, image restore for archive v>=4 and the legacy images/videos maps for v<4,
  REPLACE wipes (cascades included), and the maintenance/semantic side effects.

## Determinism

Two masking passes run over the serialized trace: generated UUIDs (the transfer id and the
backfill planner's run ids) become `<uuid:N>` by first-appearance ordinal, and every 13-digit
epoch-millisecond value (the upsert's `dataChangedAt` stamp, the maintenance/semantic
`updatedAt`) becomes `<clock>`. Fixture rows never carry UUID-shaped or epoch-sized values, so
the substitutions are unambiguous. One scenario pins the kotlinx decode failure path
(`MissingFieldException` text without a path suffix, because `decodeFromJsonElement` has no
source).

## Running

```sh
JAVA_HOME=<jdk17> KOTLINC=<kotlinc-with-serialization-plugin> \
KOTLINX_JARS=<commons-compress|commons-io|kotlinx-serialization|coroutines|sqlite-jdbc jars> \
./run_golden.sh --freeze   # regenerate traces (first run)
./run_golden.sh            # verify against the frozen traces
```

`javac` must be on `PATH` (it compiles the vendored Android JSON sources).
