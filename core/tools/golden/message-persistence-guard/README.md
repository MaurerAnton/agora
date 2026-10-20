# message-persistence-guard golden stand

Pins `model/MessagePersistenceGuard.kt`: the per-column text cap and the bounded JSON encoder that
keeps one message row below the CursorWindow limit without turning protected Responses
continuation state into SQL NULL.

Compiled verbatim from the app tree:

- `model/MessagePersistenceGuard.kt`
- `util/Constants.kt`

`MessageSegment`, `ToolImageAttachment` and `StreamingTextDelta` live in `model/ChatMessage.kt`,
which cannot compile host-side (Compose marker plus UI state). They are lifted out byte for byte by
the sibling `backup-media-policy/extract_message_segment.py`; only the `@Immutable` marker is a
stand-local platform stub. The generated declarations are decoded and re-encoded with the real
kotlinx serializer, so the guard is exercised on the exact model the app persists.

## What is frozen

65 traces cover:

- null/empty segment lists, canonical byte-for-byte output for ordinary rows, and rich segments
  containing every serialized field, tool images, escapes, control characters and emoji;
- the ordinary fast path versus an aggregate whose metadata pushes an otherwise-fitting payload
  over budget;
- largest-field trimming for `toolResult`, `toolResultText`, `toolStructuredResult`,
  `toolProgress` and non-tool `content`, including the tie priority and cross-segment
  first-maximum selection;
- the 2000-UTF-16-unit floor, strict reduction each iteration, termination on untrimmable rows,
  and fail-closed `NULL` for oversized metadata, zero and negative budgets;
- `tool` content exclusion from both the payload estimate and trimming, while preserving it in the
  encoded output;
- the default 600000-byte budget;
- Kotlin UTF-16 lengths, JSON escape costs, surrogate-pair boundaries, and the exact truncation
  marker;
- protected `responseOutputItems`: explicit `IllegalStateException` when continuation state cannot
  fit, preservation when it can;
- `clipText` at, above and below the 100000-unit cap, including astral boundaries and empty text;
- a deterministic 24-case differential corpus mixing trimmable fields, Unicode, escapes and budgets
  from 0 to 8000 bytes, including rows that fit, trim once, trim repeatedly and fail closed.

## Run

```bash
export JAVA_HOME=...
export PATH="$JAVA_HOME/bin:$PATH"
export KOTLINC=/path/to/kotlinc
export KOTLINX_JARS=/path/to/kotlinx-serialization-core-jvm.jar:/path/to/kotlinx-serialization-json-jvm.jar
./run_golden.sh            # verify against golden/
./run_golden.sh --freeze   # regenerate after changing scenarios or the Kotlin source
```

C++ parity: `core/tests/message_persistence_guard_parity_test.cpp`
(`message_persistence_guard_golden_parity`). The guard reuses the backup codec's
`encode_message_segments`; that encoder is separately pinned byte-for-byte by the
`backup-media-policy` stand.
