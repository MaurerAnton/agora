# native-backup-archive golden stand

Parity pin for `data/NativeBackupArchive.kt` — the on-demand reader every `.agora` import opens:
central-directory validation, byte-verified in-memory metadata, lazy checked streams, resource
extraction and the storage preflight. The archive source and its collaborators compile
**verbatim from the app tree**; the C++ port (`agora::core::native_backup_archive`, test
`core/tests/native_backup_archive_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 36 op sequences |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `fixtures/*.zip` | The archives both ports read (24 committed, byte-exact) |
| `NativeBackupArchiveTraceDump.kt` | The stand driver (calls the real archive) |
| `gen_scenarios.py` | Builds fixtures + scenarios deterministically |
| `agora_extract graph-interface` | Lifts `NativeGraphEntrySource` verbatim from `NativeConversationGraphSource.kt` |
| `platform/Uri.kt`, `platform/AndroidContext.kt`, `platform/ParcelFileDescriptor.kt` | The Android open-path surface |
| `platform/SettingsManager.kt`, `Preferences.kt`, `SecretCrypto.kt`, `DebugLog.kt` | The DataStore storage double needed so `SettingsContracts.kt` compiles (same files as the secrets stand) |
| `run_golden.sh` | Generates fixtures/scenarios, compiles, freezes or verifies the traces |

## What is pinned

* **The three open paths** — `open(context, uri)` (file descriptor → seekable channel),
  the internal `open(temporaryFile)`, and `open(SeekableByteChannel)` — including:
  * `null` when the content resolver has no descriptor;
  * the JDK `FileNotFoundException` text `<path> (No such file or directory)`;
  * `Backup source does not support random access; download it to local storage and select the local file`
    for a channel without `position()`;
  * the owned temporary file is deleted by every failed open and again on `close()`;
  * `Invalid backup archive` for a platform failure that is not an `IOException` (the pinned case:
    a negative metadata limit).
* **commons-compress' own failure surface** — a central directory that cannot be read (truncated,
  garbage, hand-built structural breakage) is reported as
  `Error reading Zip content from <label>`, where the label is the channel class:
  `FileChannelImpl` for file/PFD streams, `BytesChannel` for the stand's in-memory channel.
* **Every Kotlin entry-path guard**, including the interaction with commons' normalization of raw
  backslashes (the raw-name check fires first and prints the *normalized* name):
  duplicates, parent-as-file ambiguity, `Unsafe ZIP entry path` for empty/absolute/`//`/`./`/`../`
  names, the Windows drive-letter ambiguity, and `ZIP directory entry contains data`.
* **Metadata verification at open**: the 256 MiB byte budget (with its fixed message even for a
  smaller configured limit), declared-size vs payload (`read more` / `read N`), and CRC mismatches
  (`<name> CRC mismatch`) — streamed payloads (`conversations.json`, `conv/tasks.json`,
  `conv/items/*`) are *not* read at open.
* **The stream matrix**: resource rejection (`Resource entry must be copied to storage`),
  `bytes()` rejection of streamed payloads (`Streamed payload must be read as a stream`),
  `prefix()` working on *any* entry, missing-entry `null`s, strict-UTF-8/no-control rendering
  (`hex:` for binaries), verification exactly when the end of the data is reached, early `close()`
  never verifying, and `skip()` reading through the bytes it skips.
* **Damaged DEFLATE content**: surfaced as zlib's own text (`invalid stored block lengths`) at the
  first read — the same words the pinned zip-reader stand reproduces — never at `stream()`
  construction.
* **copyTo / copyStreamToFile**: null for a missing entry, `<entry> exceeds the import size
  limit`, `Insufficient storage for <path>: N bytes required, M available`, parent creation,
  the `bufferBytes > 0` require, size/CRC verification, and `target` deletion on every failure.
* **preflightImportResources**: spool bytes (v5 conversation items + task list), selected
  conversation resources, the v4/v<4 font selection, the legacy-font `customFontLimitBytes`
  exemption, both overflow messages, and the storage check (destination paths masked as `<dir>`).
* **The driver's state machine** (stand logic, mirrored by the C++ test): operations after a failed
  open report `Archive is not open`, stream ops on an unopened key report
  `Stream not open: <key>`.

## Fixtures

`gen_scenarios.py` builds them two ways:

* `zipfile` with fixed 1980 timestamps — the normal archives the functional scenarios open;
* `RawZip`, a byte-exact assembler — wrong central CRCs and sizes (with a central ZIP64 extra
  where the size exceeds 4 GiB), unsafe names, a directory entry with data, sizes near
  `Long.MAX/2` for the overflow messages, a truncated DEFLATE stored block, garbage and a
  truncated archive.

## Scenario format

```json
{ "name": "...", "archive": "valid-v6.zip", "setupFiles": ["blocker"],
  "ops": [ {"op": "open", "mode": "file|uri|uri_null|channel|nonseek", "metadataLimit": 40},
           {"op": "names"}, {"op": "bytes", "name": "manifest.json"},
           {"op": "stream_open", "key": "s", "name": "conv/tasks.json"},
           {"op": "stream_read", "key": "s", "count": 6},
           {"op": "copy_to", "name": "...", "target": "...", "maxBytes": 1, "available": 2},
           {"op": "preflight", "conversations": true, "settings": false, "version": 6,
            "dest": "d", "fontLimit": 0, "available": 40},
           {"op": "temp_exists"}, {"op": "close"} ] }
```

Traces are line groups: `op|<index>|<type>` then the result lines
(`names|<n>` + `entry|<name>`, `bytes|<n>|<text|hex:…>`, `copy|…`, `preflight|<n>`,
`exists|true|false`, `stream|<key>|opened`, `read|<n>|…`, …) or `error|<message>`.
Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\` `\p` `\s` `\n` `\r` `\t`; absolute scenario paths are
masked as `<dir>` because the C++ test runs from a different working directory.

## Boundaries

* **commons-compress → `zip::Reader`**: the stand runs the real commons `ZipFile`; the port reads
  the central directory with the already-pinned zip reader and streams `STORED` entries by their
  local header (what `ZipFile.getInputStream` delivers) and DEFLATED entries through the pinned
  RFC 1951 decoder. Commons' constructor failures are reproduced as the channel-label message;
  commons' internal local/central *overlap* validation has no observable case in the corpus.
* **`source_label`** is an explicit platform input: `FileChannelImpl` for file and file-descriptor
  channels, the stand channel's class name for in-memory channels.
* **Streaming is in-memory** (the zip-reader stand's documented boundary), but the *ordering* is
  faithful: content is inflated on the first read, so a damaged streamed payload opens fine and
  fails when it is read.
* **The Android open-path surface** (`Context`, `Uri`, `ParcelFileDescriptor`) is substituted by
  three small platform files; `NativeGraphEntrySource` is lifted verbatim by
  `agora_extract graph-interface` because the rest of `NativeConversationGraphSource.kt` needs
  `android.util.JsonReader`.
* **`usableSpace` defaults**: the driver passes `Long.MAX_VALUE` for the `availableBytes`
  default lambda; every scenario whose outcome depends on the value passes it explicitly, so the
  substitution is equivalent for the pinned sizes.
* Kotlin's `entry.crc < 0` guard is unreachable (an unsigned central-directory CRC widened to
  `Long`); the port keeps the `entry.size < 0` fail-closed guard and documents the CRC one.

## Regenerating

```bash
KOTLINC=/path/to/kotlinc \
KOTLINX_JARS=core.jar:json.jar:coroutines.jar:/path/commons-compress-1.27.1.jar:/path/commons-io-2.16.1.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```

`run_golden.sh` rebuilds `fixtures/` and `scenarios/` from `gen_scenarios.py` on every run
(deterministically), lifts the interface, compiles the app sources, and runs each scenario in its
own working directory under `build/` (git-ignored).
