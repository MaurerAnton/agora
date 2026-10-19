# backup-media-policy golden stand

Parity pin for the native port of `data/NativeBackupMediaPolicy.kt`. Every scenario under
`scenarios/` runs through the **real Kotlin policy compiled verbatim from the app tree** and the
canonical trace is frozen under `golden/`; the C++ port
(`agora::core::backup::NativeBackupMediaPolicy`, test
`core/tests/backup_media_policy_parity_test.cpp`) must reproduce every trace byte for byte.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | One case per file, 112 frozen edge cases |
| `golden/*.trace` | Frozen canonical output, one per scenario |
| `BackupMediaTraceDump.kt` | The stand driver (decodes a scenario, calls the policy, prints the trace) |
| `extract_message_segment.py` | Lifts `MessageSegment`, `ToolImageAttachment` and `StreamingTextDelta` out of `model/ChatMessage.kt` |
| `PlatformStubs.kt` | The `@Immutable` Compose marker annotation |
| `run_golden.sh` | Extracts, compiles and regenerates or verifies the traces |
| `gen_scenarios.py` | Generates the scenarios (deterministic; rerun after editing) |

## What is pinned

The policy decides which attachment bytes a native backup may carry, and how a restored draft
points at them again. It is pure — model data classes in, one JSON string out, with four
callbacks in between — so everything observable fits in one trace.

**The call log is the point of most of these scenarios.** Kotlin resolves its candidate sources
with `firstNotNullOfOrNull`, which stops at the first hit. A port that probes in a different order
returns the same value while asking the archive layer for different paths — a real bug in a backup
format that result-only testing cannot see. The scenarios pin:

* `draft_export_short_circuits_on_local_path` — the URI entry is in the table and is never asked
  for, because the local path already resolved.
* `draft_export_probes_blank_local_path_before_the_uri` — a blank local path is still a list
  element, so `archiveEntryForSource(" ")` really is called before the URI.

**`sourceFileName` is not a basename operation.** It strips a `?` query and a `#` fragment,
trims trailing `/` and `\`, then takes the text after the last `/` *and* after the last `\` — and
`substringAfterLast` returns the **whole string** when the delimiter is absent. Three scenarios
cover the cases where a naive port returns the wrong thing: a Windows path with no `/`, a name
with no separator at all, and `a/b\c`, whose name is `c` rather than `b\c`. A name that trims to
Kotlin-whitespace (`U+00A0`) is rejected and the search falls through to the next candidate.

**Availability is decided per attachment type**, and each rule has its own scenario: a `video`
needs an archived payload, a `file` needs its text, an `image` or `pdf` needs at least one
surviving media index, and **any other type is always available**. An unavailable item loses its
text content, its index and its page count, and is counted once for `onMissingResource`.

**Export and restore are not symmetric, and both directions are pinned:**

* Only a video keeps a structural `originalUri`, and only while its payload is archived. An image
  drops its device URI but keeps the file name *derived from it*.
* `pageCount` is recomputed as the number of surviving indices, so it can shrink; a page count
  below one is coerced to one, which is what stops a zero from claiming no indices at all.
* Restore forces `APP_PRIVATE` storage and clears the sandbox path; export never sets storage at
  all, and simply filters sandbox items out.
* `archiveVersion < 4` restores a video from `legacyVideoUris[imageIndex ?: 0]` — the `: 0`
  fallback is its own scenario — while `archiveVersion >= 4` ignores the legacy table entirely.
* Below v4, tool-image thumbnails are dropped **without probing**, because v1–v3 stored only a
  device-absolute path and a misleading foreign path is worse than a missing thumbnail.

**kotlinx's encoder is pinned too**, since these traces are JSON in a JSON string. Field order is
the Kotlin declaration order, defaults are omitted (`encodeDefaults = false`), an explicitly empty
nullable collection is still written as `[]` while an empty one whose default is `[]` is dropped,
an explicit `null` for a non-nullable property fails the decode, and the escapes are exactly
kotlinx's: the five short forms, lowercase `\u00xx` for the rest of C0, UTF-8 passed through
untouched.

## Scenario format (`scenarios/*.json`)

```json
{
  "name": "...",
  "fn": "rewriteAttachmentMetaForExport",
  "raw_json": {"items": []},
  "attachments": [{"localId": "a1", "uri": "content://...", "type": "image"}],
  "originalImageSources": ["/orig/one.png"],
  "oldToNewImageIndex": {"0": 1},
  "legacyVideoUris": {"0": "/device/legacy.mp4"},
  "archiveVersion": 4,
  "table": {"archiveEntryForSource": {"content://...": "media/one.png"}}
}
```

* `fn` selects which policy function runs; the remaining fields are its arguments.
* `raw` is a literal string (so it can be deliberately malformed) and `raw_json` is a JSON value
  that the driver re-serializes; `raw_json: null` is an explicit SQL-style null.
* `table` drives the callbacks. A JSON `null` value means the callback resolves to null; a
  `restoredPrimaryForArchiveEntry` entry is a two-element array because it returns a pair.
* An empty `table` means the callback is never expected to resolve.

## Trace format (`golden/*.trace`)

```
name|<scenario name>
fn|<function name>
call|<callback>|<escaped argument>|<escaped result or ->
call|<callback>|<escaped argument>|<escaped local>|<escaped uri>   (restoredPrimaryForArchiveEntry)
result|attachments|<escaped JSON array>
result|strings|<escaped JSON array>
result|string|<escaped string or ->
missing|<count of onMissingResource calls>
```

Escaping: `\` `|` `;` `\n` `\r` `\t` → `\\\\` `\\p` `\\s` `\\n` `\\r` `\\t`; a null result is `-`.

## Scope and boundaries

* `NativeBackupMediaPolicy.kt` and `model/AttachmentMeta.kt` compile **verbatim**. `MessageSegment`,
  `ToolImageAttachment` and `StreamingTextDelta` live in `model/ChatMessage.kt`, which cannot compile
  host-side (Compose's `@Immutable`, UI state, Room), so `extract_message_segment.py` copies those
  three declarations out **verbatim** rather than retyping them — retyping would pin the
  transcription instead of the implementation. The extractor fails loudly if any of the three
  changes shape, and the stand README records that the blocks are byte-identical to the source.
* `SelectedAttachment.localId` defaults to a random UUID, so every scenario sets it explicitly.
* **Duplicate JSON keys are not exercised.** The Kotlin decoder and this port both take the last
  assignment, but no scenario depends on it.
* The stand pins the policy's decisions and the exact JSON it produces. It does not pin what the
  archive layer does with the entries it is asked for — the callbacks are tables here by
  construction.

## Regenerating

```bash
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc-2.3.21 KOTLINX_JARS=core-1.11.0.jar:json-1.11.0.jar ./run_golden.sh          # verify
JAVA_HOME=/path/to/jdk17 KOTLINC=/path/to/kotlinc-2.3.21 KOTLINX_JARS=core-1.11.0.jar:json-1.11.0.jar ./run_golden.sh --freeze # regenerate
python3 gen_scenarios.py                                                                                                     # rewrite scenarios
```

**The toolchain must match the app: Kotlin 2.3.21 and kotlinx-serialization 1.11.0.** That is a
requirement rather than a preference. `NativeBackupMediaPolicy.kt` calls
`json.encodeToString(AttachmentMeta(items))` without importing `kotlinx.serialization.encodeToString`,
which compiles only because 1.11.0 exposes `encodeToString(T)` as a reified **member** of `Json`;
on the older 1.6.3 the file does not compile at all. Matching versions is what lets the stand
compile the source verbatim instead of patching an import into it.

`run_golden.sh` wipes the stand's `build/` (its git-ignored scratch, including the extracted
`MessageSegment.kt`) before each run, so runs are idempotent.