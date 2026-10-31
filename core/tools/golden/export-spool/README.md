# export-spool golden stand

Parity pin for `data/ExportSpool.kt` — the newline-delimited export spool, the layout index it
produces and the streaming reader that revisits one conversation or task block at a time.

`ExportSpool.kt` compiles **verbatim from the app tree**; it has no dependencies beyond
`java.io`, so the stand needs no platform substitute at all. The driver replays a writer op
sequence and/or reader probes against a real spool file (`platform_spool.tmp`).

## Layout

| Path | Purpose |
| --- | --- |
| `scenarios/*.json` | 31 cases: writer sequences, raw spool content, reader probes, deterministic fuzz |
| `golden/*.trace` | Frozen Kotlin output, one per scenario |
| `ExportSpoolTraceDump.kt` | Driver: writes the spool, renders the index and runs the reads |
| `agora_scenarios export-spool <stand>` | Deterministic scenario generator |
| `run_golden.sh` | Compiles, freezes (`--freeze`) or verifies the traces |

## What is pinned

* **The frame format**: `TYPE \t payload \n` with `TYPE` in `C/R/M/T/L`; the payload is written
  verbatim, so a payload that contains a newline really does break the framing (scenario 08).
* **The layout index**: the byte offsets of every conversation slice and task range, and the
  `watermarks` map. The offsets come from Kotlin's `CountingOutputStream`, which only counts
  *flushed* bytes — the writer flushes before every offset read (conversation close, task close),
  and the port mirrors those flush points exactly. Scenario 03 pins the UTF-8 byte math (a
  conversation with `юникод 🙂` payloads ends at byte 84, not at character count).
* **The writer state machine**: `writeConversation` closes the open conversation *and* the open
  task block, `writeTask` closes the conversation but keeps its own range open, and a run/message/
  loop record outside a conversation throws
  `IllegalStateException("Spool record R emitted outside a conversation")` — the record type is
  part of the message (scenarios 04/05 and the fuzz corpus).
* **An aborted writer stops the export**: the trace keeps the bytes written so far, records the
  error and pins the reads as skipped (no index exists to resolve a symbolic slice against).
* **`readRecords`**: the slice end is clamped to the file length, a slice that ends before its
  start reads nothing, a trailing record without a newline is still emitted, and a malformed line
  (no tab, or a tab at position 0) throws `IOException("Invalid export spool record")` *after* the
  records before it were delivered (scenario 23).
* **The record iterator**: the type filter, the chunked reads (64 KiB) and the carry buffer that
  lets a record span chunk *and* slice boundaries (scenario 25 splits a record across two
  slices), the unterminated last line, empty slices being skipped, and the exhausted state machine
  — after a malformed record the iterator is exhausted, `hasNext()` is `false` and `next()` throws
  `NoSuchElementException("Export spool iterator exhausted")` (scenario 23 pins all three).

## Boundaries

* The spool is a real file on both sides (the Kotlin driver writes `platform_spool.tmp`, the C++
  test writes the same name in its working directory). Nothing about the file layout is simulated.
* Record payloads are split at the tab **byte**: the tab is ASCII, so a UTF-8 continuation byte
  can never be mistaken for it, and the byte index equals the Kotlin `String` index. The corpus
  therefore keeps payloads valid UTF-8 (the JVM would repair invalid bytes with `?`).
* The 64 KiB chunk size is an internal detail of both implementations; it is pinned only through
  the records that cross a chunk boundary (scenario 09/30).

## Running

```bash
export JAVA_HOME=/home/user/jdk17; export PATH=$JAVA_HOME/bin:$PATH
KOTLINC=/home/user/agora-toolchain/kotlinc-2.3.21/bin/kotlinc \
KOTLINX_JARS=/home/user/agora-toolchain/jars/kotlinx-serialization-core-jvm-1.11.0.jar:/home/user/agora-toolchain/jars/kotlinx-serialization-json-jvm-1.11.0.jar \
./run_golden.sh            # verify
# add --freeze to regenerate golden/
```
