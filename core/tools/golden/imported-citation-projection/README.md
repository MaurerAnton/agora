# imported-citation-projection golden stand

Parity pin for `data/ImportedCitationProjection.kt` — the citation half of the ChatGPT/Claude
import transforms: marker stripping + reference resolution (`projectChatGptCitations`), the single
reference projection (`projectClaudeCitation`) and the bounded row encode
(`encodeImportedCitations`).

`ImportedCitationProjection.kt`, `model/Citation.kt`, `model/MessagePersistenceGuard.kt`,
`model/ChatMessage.kt` (+ its leaf models and `util/Constants.kt`) compile **verbatim from the app
tree**; ChatMessage's compile-only `@Immutable`/markdown annotations use the documented leaf stubs
shared with the `citations` stand.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 51 cases across the three ops (markers, anchors, references, budget, fuzz) |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `ImportedCitationProjectionTraceDump.kt` | Driver: runs one op per scenario against the record renderer |
| `agora_scenarios imported-citation-projection` | Deterministic scenario generator (real U+E200/U+E201/U+E202 envelopes) |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The two marker regexes** over UTF-16 code units: `\uE200(cite|filecite)\uE202([^\uE200\uE201]+)\uE201`
  and `【turn\d+[a-z]+\d+)】` (case-insensitive, leftmost, non-overlapping), including the split on
  U+E202, Kotlin `trim` + `isNotBlank` filtering of the id parts, the `ifBlank { "cite" }` kind
  fallback and the "blank group-2 falls back to group-3" rule.
* **Position arithmetic in UTF-16 units**: `markerEnd` is recorded against the *pre-strip* cleaned
  text while anchors resolve against the *post-strip* answer — scenarios 12/13/22 pin what happens
  when those spaces disagree.
* **Reference walking order**: `collectPrivateIds` (private `turn…` ids inside string content) →
  `collectStructuredRefIds` (`turn_index`+`ref_type`+`ref_index`) → `uuid`/`source_id`;
  `findNamedString` checks the name list before recursing (per object), `findSafeUrlCandidate`
  consults `safe_urls` first — any primitive contributes its raw content (a number becomes a
  candidate!), then falls back to the recursive walk.
* **Source gating**: a reference without ids/url/fileName/title never produces a citation, and a
  marker id that no source carries is dropped from the record list while the marker itself is
  still stripped from the answer.
* **The three anchor branches**: exact `start_idx`/`end_idx` range (scenario 27 pins the trimmed
  string form of the `intOrNull` accessor, scenario 28 pins `"8.0" -> null`), the
  `lastIndexOf(matched, markerEnd)` fallback gated on a blank gap (12), and both failure exits
  (mismatch 13, out-of-range 14, markerEnd beyond the stripped text 22).
* **`intOrNull` grammar** for `turn_index`/`ref_index` (`"3e2"` → 300, scenario 29 — the same
  JSON-literal grammar the export-extra-settings stand pins).
* **Claude projection**: the `answerStartIndex` override branch (anchor spans the whole block)
  only when the answer is non-empty, `providerSourceId` from `ids.firstOrNull()`, `forceFile` from
  `fileName`.
* **`encodeImportedCitations`**: dedup → `CitationRecord.toMessageSegment` → the pinned
  `MessagePersistenceGuard.encodeSegmentsBounded` — including scenario 44, where six records with
  32×4096-char anchors exceed the 600000-byte budget and the guard trims the citation segments
  down to the frozen shape.

## Boundaries

* `CitationPolicy` (create/encode/deduplicate/strip/safeHttpUrl), `MessagePersistenceGuard` and
  `MessageSegment` are **not** reimplemented — the port calls the already-pinned `citations`,
  `message_persistence_guard` and `backup` modules; this stand pins only the projection glue.
* Id lookup lowercases keys; Kotlin `lowercase()` is Unicode-aware while the port folds ASCII —
  marker ids are ASCII (`turn…`/uuid), so the distinction is unreachable for real exports.
* The `encode` op's input records are injected by the driver and decoded with the real
  `@Serializable` serializer (declared defaults included); scenarios always use properly typed
  fields, matching what `CitationPolicy.create` produces.
* UTF-8 is the boundary: a lone surrogate cannot be represented and becomes `?` exactly like the
  JVM's UTF-8 encoder (same rule as the citations stand).

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
