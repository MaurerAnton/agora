# run-ui-projection golden stand

Parity pin for `model/MessageGenerationBoundary.kt` and `model/RunUiProjection.kt` — the message
generation boundaries and the UI affordances (Regenerate / Edit / copy / delete) derived from them.

Both files compile **verbatim from the app tree**, together with `model/ChatMessage.kt` (whose
compile-only Compose/markdown annotations use the `citations` stand's documented leaf stubs).

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 26 cases: boundaries, branches, protocol rows, run groups, selection, fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `RunUiProjectionTraceDump.kt` | Driver: resolves the boundaries and renders the presentation map |
| `gen_scenarios.py` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The boundary resolver**: a real USER row is a hard boundary; rows that carry a durable Run id
  stay together (a different Run id closes the group); the boundary exposes its input (the first
  real USER row), its first and its last ordinary assistant row. Protocol rows (`tool_`/`result_`
  prefixes) and context-compact rows are never real USER or ordinary assistant rows.
* **The identity rule**: the id is the structural identity — a duplicate id (a transient
  Room/optimistic-commit race) must not be read as two real branches, so both the visible and the
  full list are de-duplicated by id.
* **The two branch dimensions**: edited USER rows are siblings under the *preceding* message (their
  selector belongs to the generation's input), regenerated MODEL roots are siblings under one shared
  USER input (their selector belongs to the selected generation's terminal output). The sibling
  order is `timestamp` then `id`.
* **The presentation map**: every visible message gets an entry; a real USER row and the last
  ordinary assistant row of a boundary expose the action bar, the copy text (only when the text is
  not blank), the delete target, the branch selector (`totalBranches > 1`), the branch index and the
  branch anchor. The delete target of an assistant row is the *root* output of its branch, not the
  selected one.
* **The structural input fallback**: when the boundary's input is not the parent of the root
  output, the parent is looked up in the full message list (and must be a real USER row).

## Boundaries

* The chat model is the already-ported `message_preparation::ChatMessage` subset (the display-only
  fields the projection never reads are not modelled).
* The blank checks follow Kotlin's `isNullOrBlank` with ASCII whitespace; the corpus uses ASCII
  whitespace only.

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
