# data-importer

Golden pin for `app/.../data/DataImporter.kt` — the archive `preview(uri)` /
`import(uri, decisions)` entry points plus the `FontUtils` reads that sit next to
them. Every scenario drives a ZIP assembled from its ordered entry list; the
trace records what the import actually did.

## Boundaries (what is real, what is substituted)

`DataImporter.kt` and everything it calls compile **verbatim** from the app tree:
the conversation-graph chain (source → importer → media restorer),
`NativeBackupArchive`, `PortableSettingsArchive`, `ExportExtraSettings`,
`NativeBackupSecretsPolicy`, `MemoryManager`/`SkillManager`, `FontUtils`,
`ConversationSettingsTransferCoordinator`/`ConversationSettingsState` and the
ChatDao/entity cluster. Four boundaries are substituted:

1. `android.util.JsonReader/JsonWriter/JsonScope/MalformedJsonException` +
   `StringPool` — vendored verbatim from AOSP by the
   `native-conversation-graph-source` stand, compiled with `javac`;
2. `DataExporter.ExportCategory` — lifted byte-for-byte by
   `agora_extract export-category` (it is the decision-map key; the export root
   is not pulled in);
3. the Android/Room surface — `Context`/`ContentResolver` (with the archive
   descriptor path), `ParcelFileDescriptor`, `FileProvider`,
   `MaintenanceDebtWorker.schedule`, and the stand `ChatDatabase` over the
   Room-generated schema-36 database;
4. the DataStore storage classes — `SettingsManager` (the data-exporter stand's
   recording double, extended with the read-back state the importer observes),
   `SettingsRepository` (the real `ConversationSettingsState.applyImport` over a
   recording persistence boundary) and `ConversationRepository` (verbatim ChatDao
   delegation).

## Layout

| File | Role |
| --- | --- |
| `DataImporterTraceDump.kt` | Driver: `preview` / `import` / `font` steps, masking, `<root>` substitution |
| `DataImporterSettingsSnapshot.kt` | SettingsManager snapshot construction and archive writing |
| `DataImporterSettingsView.kt` | Settings/memory/skills/files trace rendering |
| `DataImporterTableDump.kt` | Database table dump for the trace |
| `platform/` | Android/Room doubles and the stand ChatDatabase |
| `scenarios/*.json` | 24 inputs: ordered archive entries + `settings` + `appVersion` |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `run_golden.sh` | Generates scenarios, extracts, compiles, verifies (or `--freeze`) |

## Scenarios (24 files, 31 steps: 6 preview / 22 import / 3 font)

| Group | Scenarios |
| --- | --- |
| Preview | `01-preview-v6`, `02-preview-legacy`, `03-preview-invalid` (3 bad-version paths), `21-preview-count-errors` |
| System prompts | `04-import-prompts-merge`, `05-import-prompts-replace` |
| Conversation graph | `06-import-graph-merge`, `07-import-graph-replace`, `08-import-graph-failure` |
| Memories | `10-import-memories-merge`, `11-import-memories-replace`, `20-import-memory-error` |
| Settings & keys | `12-import-settings-v6`, `13-import-settings-v3`, `14-import-settings-error`, `15-import-keys`, `16-import-all` |
| Fonts | `09-font-utils` (direct reads), `23-import-font-limit`, `24-import-font-fallback` |
| Failure & decisions | `17-import-errors` (4 steps), `18-import-pending-failure`, `19-import-complete-failure`, `22-import-no-decisions` |

A trace line pins, per step: the preview result or import report, the filtered
`SettingsManager` call log (only the calls DataImporter's own orchestration and
the legacy/secrets paths make — the archive's scalar restore calls belong to the
`portable-settings-archive` stand), the settings values read back, memory/skill
state, the files tree, the database tables and the coordinator's applied
imports. Generated UUIDs and wall-clock stamps are masked; the temporary root
becomes `<root>`.

## Regenerating

```bash
KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
../../../build/agora_scenarios data-importer .   # rewrite scenarios (run_golden.sh does this first)
```

## Coverage against the import TZ (fork spec)

The TZ describes the full importer (preview → validate → import → rollback).
Status of each block **in this tree**:

| TZ block | Status | Pinned by |
| --- | --- | --- |
| §3.1–3.2 Room DB import, merge/replace strategies, failure handling | covered | `06`, `07`, `08`, `16`, `17`, `19` |
| §3.3 PathMigrator (package rename over columns/URLs) | **no code in this tree** — nothing to pin yet | — |
| §3.4 SchemaMigrator (column plan, type casts, index/FK rebuild) | **no code in this tree** | — |
| §4.1–4.2 SharedPreferences/DataStore settings import | covered (legacy v3 + v6 + error) | `12`, `13`, `14`; scalar restore is pinned by `portable-settings-archive` |
| §5 FileImporter: GGUF copies + `backup_*.json` package rewrite | **no code in this tree**; only font files are handled today | fonts: `09`, `23`, `24` |
| §6 Encrypted values / API keys | covered (skipped fields policy + key import) | `15` |
| §7 Validation: row/count errors | partially covered | `21`; checksums and FK verification are not implemented |
| §7 Validation: checksums (CRC32), referential integrity | **no code in this tree** | — |
| §8 ImportOrchestrator (backup target, parallel imports, report) | **no code in this tree** — current orchestration lives inside `DataImporter` | `16`, `17`–`19` pin the current behaviour |
| §10 Rollback / restore on failure | **no code in this tree**; `18`/`19` pin failure *outcomes*, not rollback | — |
| §12 Test plan | this stand (JVM, no device) + the app unit tests in CI | — |

When the missing components land, add their scenarios next to the matching
group above: the generator is deterministic, so a new scenario needs
`agora_scenarios data-importer .` and one `run_golden.sh --freeze` pass.
