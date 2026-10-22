# generation-streaming-segments golden stand

Parity pin for `viewmodel/GenerationStreamingSegments.kt` — the live segment timeline of a
generation: the thought timing, the segment merge, the live-segment builder, the citation rebase,
the bounded output transform and the final snapshot.

The policy file is lifted **verbatim** by `extract_policy.py`, which only widens `internal` to the
default visibility so the driver can call it; `ChatMessage.kt`, `MessagePersistenceGuard.kt`,
`Citation.kt`, `TokenUsage.kt` and `Constants.kt` compile verbatim from the app tree.

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 14 cases: the timing, the merges, the live builder, the rebase, the transform, the snapshot |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `GenerationStreamingSegmentsTraceDump.kt` | Driver: runs every policy of the file |
| `extract_policy.py` | Lifts the policy file with a widened visibility |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Extracts, compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The thought timing**: the current and the total duration, the live duration, the clock rollback
  (a rollback is not due) and the adoption of a reported duration.
* **The segment merge**: two answer segments merge, two thought segments merge only while neither
  carries a signature, the merged duration is the sum (and disappears when it is not positive) and
  the streamed text deltas are concatenated.
* **The live-segment builder**: the citations are moved to the end, the answer and the thought are
  merged into the flushed segments, a blank error message is dropped and an empty result is `null`.
* **The citation rebase**: the anchors shift by the provider's answer start, an anchor that would
  overflow `Int` is dropped, and a record that no longer normalises keeps its identity but loses
  its anchors.
* **The bounded output transform**: the transformed text is clipped by the persistence guard and
  distributed over the answer segments, the last answer segment taking the remainder, and the
  streamed deltas are dropped.
* **The terminal error**: the current error is kept unless the status is an error, in which case the
  fallback is used when the current error is blank.
* **The final snapshot**: the clipped text, the blank thoughts becoming `null`, the live segments
  (or the flushed ones when the live builder is empty) and the run identity.

## Boundaries

* The chat model is the already-ported `message_preparation::ChatMessage`; the port adds the
  fields the policy reads (`token_usage`, `thought_time_ms`, `MessageSegment.duration_ms` and
  `MessageSegment.streaming_text_deltas`).
* The citation policy and the persistence guard are reused as they are — the port never
  re-implements `CitationPolicy.normalize` or `clipText`.
* The clock is injected, so the timing is deterministic on both sides.
