# claude-chat-importer golden stand

Parity pin for `data/ClaudeChatImporter.kt` — the "import a Claude chat export" path: picking the
conversations out of the export bytes, the preview the import dialog shows and the chat/message
entities the database import writes.

`ClaudeChatImporter.kt`, `ImportStreams.kt`, `ImportedCitationProjection.kt`, `model/Citation.kt`,
`model/MessagePersistenceGuard.kt` and `model/ChatMessage.kt` compile **verbatim from the app tree**
(ChatMessage's compile-only annotations use the `citations` stand's documented leaf stubs). The
citation projection and the attachment encoder are the already-pinned modules — the stand pins the
importer's own glue, not their policies.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 31 cases: raw JSON exports, ZIP archives, decode failures, selection, attachments |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `fixtures/*.zip` | The archives the driver built (committed so the C++ port reads the same bytes) |
| `ClaudeChatImporterTraceDump.kt` | Driver: feeds the importer and renders preview + import format |
| `agora_scenarios claude-chat-importer` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces and fixtures |

## What is pinned

* **The archive step**: the `PK` probe, the ZIP walk that prefers the entry whose name contains
  `conversation` (case-insensitive) and otherwise falls back to the first entry that decodes to
  conversations, the raw-document path with its `{"conversations": [...]}` and bare `[...]` shapes,
  the leading-whitespace peek and the `Unrecognized JSON format` failure, plus
  `No conversation data found in ZIP archive`.
* **The decoder's configuration** (`Json { ignoreUnknownKeys = true; isLenient = true;
  coerceInputValues = true }`): unknown keys are ignored, a primitive is accepted where a string is
  expected (the number 42 becomes `"42"`), a null becomes the field's default (`"name": null` →
  `""`) and a shape mismatch fails the whole document (the trace reports the `decode` category
  without kotlinx's message).
* **The preview**: the summaries sorted by `updated_at` descending (stable for equal timestamps),
  the title chain (`name` → first 50 characters of `summary` → `Untitled`), the message counts per
  sender and the "has attachments" flag (attachments *or* files).
* **The import format**: one chat entity per selected conversation and one message entity per
  message, the content-block merge (thinking blocks are excluded from the text but become
  `thoughts`), the citation projection with the block offsets (UTF-16 indexes, as in Kotlin), the
  attachment metadata JSON (image/file detection by MIME type and extension, the extracted text only
  for text files), the zero-UUID parent reset, `USER`/`MODEL` participants and the ISO-8601
  timestamps.
* **The clock fallback**: `iso8601ToMillis` falls back to `System.currentTimeMillis()` for a text
  that is not an ISO instant, so the driver masks every timestamp inside the run window as
  `<now>` — the C++ test masks its own window the same way.

## Boundaries

* The decoder mirrors kotlinx's lenient mode for the shapes an export can contain. The corpus keeps
  the documents to plain JSON (quoted strings, real numbers), which is what a Claude export is;
  kotlinx's single-quoted/unquoted lenient forms are not exercised.
* ZIP reading is the pinned `zip-reader` module (the JDK's `ZipInputStream` behaviour), so the
  archive failure messages come from there.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
COROUTINES_JAR=/home/user/agora-toolchain/kotlinc-2.3.21/lib/kotlinx-coroutines-core-jvm.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/ and fixtures/
```
