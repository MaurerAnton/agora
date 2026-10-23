# native-backup-v5 golden stand

Parity pin for `data/NativeBackupV5Writer.kt` and `data/NativeBackupV5Baseline.kt` — the
incremental export's archive writer and the previous-baseline reader it reuses entries from.

Both files compile **verbatim from the app tree**, together with `data/NativeBackupFormat.kt`
and the shared settings models it needs. The platform boundary is Apache Commons Compress
(`ZipArchiveOutputStream` / `ZipFile`, version 1.27.1 plus its commons-io dependency): the
stand writes real archives and reads them back, so every trace pins the archive at the entry
level — name, content, CRC, uncompressed size and method — which is exactly the surface the
already-pinned core `zip_reader` and the importers consume.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 8 cases: entry assembly, baseline validation, reuse decisions, raw copies |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `NativeBackupV5TraceDump.kt` | Driver: builds archives, replays baseline queries, dumps entries |
| `gen_scenarios.py` | Deterministic scenario generator |
| `platform/` | Identical stubs used by the `native-backup-format` stand for the shared models |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **Entry assembly** (scenario 01): `conv/items/<hex-id>.json` is
  `{"conversations":[<json>],"runs":[...],"messages":[...],"loops":[...]}` with records joined by
  `,` and no separator for an empty list; `conv/tasks.json` is `{"tasks":[...]}`;
  `conv/index.json` is the kotlinx encoding of `NativeConversationIndex` with the default
  configuration (`encodeDefaults = false`, so an empty `mediaEntries` is omitted) and the
  declaration field order `id, dataChangedAt, entry, mediaEntries`.
* **Sorted media lists**: `writeConversationEntry` returns `mediaEntries.sorted()`, so the call
  that is handed `[b, a]` indexes `[a, b]`.
* **Baseline validation** (scenarios 02/03): `openOrNull` refuses a missing file, a manifest
  whose `agora_export_version` is not 6, an `incremental_baseline` other than 1, a missing
  `conversations` category, malformed manifest or index JSON, duplicate conversation ids, an
  index entry name that is not `conversationEntry(id)`, an item entry that is not in the
  archive, and a referenced media entry that is not in the archive. Each failure yields `null`;
  the valid baseline round-trips its index and answers id lookups.
* **Reuse decisions** (scenarios 04/05): `canReuse` requires the recorded `dataChangedAt` to
  match, then an integrity check of the item entry and every media entry. A CRC patched in the
  archive (local header and central directory) makes that entry fail integrity; damage is scoped
  — a damaged media entry poisons its conversation, a damaged conversation poisons only itself,
  and an untouched conversation in the same archive still reuses.
* **Raw copies** (scenarios 06/07): `copyRaw` throws `IOException("Missing baseline entry: x")`
  when the entry is absent, and `copyConversationFromBaseline` copies each referenced media entry
  only once across calls (the caller's `copiedMedia` set — including a pre-seeded entry — is
  part of the trace). The produced archives mix copied and freshly written entries and are
  dumped in full.
* **Empty archives and empty records** (scenarios 01/08): a `writeTasksEntry` with no tasks is
  `{"tasks":[]}`, and a conversation with no runs/messages/loops still emits every separator.

## Boundaries

* **The compressed byte stream belongs to the platform compressor.** The Kotlin side writes with
  Commons' DEFLATE (method 8); the C++ port writes method 8 with RFC 1951 stored blocks (core/
  carries no third-party compressor, the same rule the reader follows). The traces therefore pin
  `name`, `content`, `crc`, `size` and `method` — never `compressed_size` or the raw deflate
  bytes. Every archive the port produces is read back by the pinned `zip_reader` in the parity
  test, so the writer is exercised as a real archive producer, not a byte mock.
* **`copyRaw` carries the compressed stream untouched.** The port reads the source entry's raw
  bytes from its local header and re-emits them with the recorded metadata (`put_raw_entry`):
  name, method, CRC, compressed size and uncompressed size plus the exact compressed bytes. The
  platform-only fields of `ZipArchiveEntry` (modification time, extra field, comment) are not
  carried — neither the app's importers nor the archive dumps read them, and the Kotlin driver
  never sets them, so they carry no app behaviour.
* **Damaged baselines are fixtures, not logic.** `corruptEntry` flips the stored CRC field in
  both headers; the driver emits that archive with `"dump": false`, because its bytes are the
  test's own construction. What the module does with it (refuse reuse) is what the trace pins.
* **Metadata limits** (`MAX_METADATA_BYTES` 16 MiB for manifest and index) are enforced on both
  sides but not exercised by a scenario: a 16 MiB fixture would dwarf the trace without adding
  a distinguishable decision.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/commons-compress-1.27.1.jar:/home/user/agora-toolchain/jars/commons-io-2.16.1.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
