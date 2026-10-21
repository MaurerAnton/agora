# gpt-chat-importer golden stand

Parity pin for `data/GptChatImporter.kt` — the "import a ChatGPT export" path: rebuilding the
message graph of every conversation, the preview the import dialog shows and the chat/message
entities the database import writes.

`GptChatImporter.kt`, `ClaudeChatImporter.kt` (the shared import entities), `ImportStreams.kt`,
`ImportedCitationProjection.kt`, `model/Citation.kt`, `model/MessagePersistenceGuard.kt` and
`model/ChatMessage.kt` compile **verbatim from the app tree** (ChatMessage's compile-only
annotations use the `citations` stand's documented leaf stubs). The citation projection and the
attachment encoder are the already-pinned modules — the stand pins the importer's own glue.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 34 cases: raw exports, ZIP archives, graph shapes, merges, selection, fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `fixtures/*.zip` | The archives the driver built (committed so the C++ port reads the same bytes) |
| `GptChatImporterTraceDump.kt` | Driver: feeds the importer and renders preview + import format |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces and fixtures |

## What is pinned

* **The archive step**: the `PK` probe, the ZIP walk that reads every `conversations.json` and
  `conversations-N.json` entry (in that order), the raw-array path, and the two failures
  (`No conversation data found in ZIP archive`, `No conversations found in JSON`). A malformed entry
  fails the whole import here (unlike the Claude importer, which skips it).
* **kotlinx's decoder configuration** (`ignoreUnknownKeys`, `isLenient` — *no* `coerceInputValues`):
  unknown keys are ignored, a primitive is accepted where a string is expected, a missing key takes
  the declared default and a **null in a non-nullable field fails the document** (pinned as the
  `decode` category without kotlinx's message).
* **The message graph**: the validity pass (a node is valid when its whole ancestor chain exists
  and is acyclic — cycles and dangling parents drop the whole chain), the breadth-first order over
  the valid nodes with the JDK's `PriorityQueue` ordering (the smallest node id first), the current
  node path in front of it, the canonical node per message id and the nearest-ancestor parent.
* **The preview**: the summaries sorted by `update_time` descending (stable), the `Untitled` title
  fallback, the message counts per role and the attachments flag (`multimodal_text` or an
  `image_asset_pointer` part).
* **The import merge**: the ephemeral ancestors (thoughts, reasoning recaps, code, tool output and
  the tool role) merged into the message they belong to, the `[Image]` fallback for a blank
  multimodal text, the skipped (empty) messages, the parent cascade through the removed messages,
  the status rules (`is_complete`, `finish_details`), the model name, the thought duration and the
  citation projection (the private markers are stripped from the text and become the citation
  segments).
* **The selected branches**: the branch map of the current path, encoded as the JSON object the
  importer stores (`{"null":"m1","m1":"m3"}` — a null parent key becomes the string `null`).
* **The clock fallback**: a message without `create_time` uses `System.currentTimeMillis()`, so
  every timestamp inside the run window is masked as `<now>` on both sides.

## Boundaries

* The decoder mirrors kotlinx's lenient mode for the shapes an export can contain; the corpus keeps
  the documents to plain JSON (what a ChatGPT export is), so kotlinx's single-quoted/unquoted
  lenient forms are not exercised.
* ZIP reading is the pinned `zip-reader` module, so the archive failure messages come from there.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
COROUTINES_JAR=/home/user/agora-toolchain/kotlinc-2.3.21/lib/kotlinx-coroutines-core-jvm.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/ and fixtures/
```
