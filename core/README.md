# Agora core — the C++ logic layer

This directory is the portable half of Agora: logic that can be built and tested on a host
machine, with no Android dependencies, and that the app consumes through a thin JNI surface
(`app/src/main/cpp/core_jni.cpp`, `libagora_core.so`).

It is **not** a parallel implementation. Every module here is pinned to the Kotlin one it
replaces by parity tests that run against the shipped assets and against golden data produced by
the real Kotlin code. A port that drifts from the Kotlin counts is a failing build, not a
surprise on a phone.

## Layout

```
core/
├── CMakeLists.txt              standalone host build (C++20, no dependencies)
├── include/agora/core/         public headers
├── src/                        implementations + generated Unicode tables
├── tests/                      host test binary (ctest), fixtures from the app tree
└── tools/
    ├── UnicodeTableGen.java    emits src/unicode_tables.generated.hpp from the JVM's tables
    ├── dump_pieces.cpp         C++ half of the parity stand
    └── golden/                 corpus, golden counts, Kotlin dumper, parity script
```

## Building and testing on a host

```sh
cmake -S core -B core-build -DCMAKE_BUILD_TYPE=Release
cmake --build core-build
ctest --test-dir core-build --output-on-failure
```

The binaries are `core-build/tests/agora_core_tests` and the parity tool
`core-build/agora_core_dump`. The tests read fixtures straight from the app tree, so run the
commands from the repository root.

## How parity is held

1. `app/src/test/resources/tokenizers/o200k_ref.json` is tiktoken's own reference set, produced
   offline with tiktoken 0.12.0. Both the Kotlin tests and the C++ tests count these texts and
   must agree with it.
2. `core/tools/golden/corpus.b64` holds 2551 texts (real repository text plus structured fuzz
   over letters, marks, digits, whitespace, contractions, punctuation and astral code points).
   `golden_counts.txt` holds what the Kotlin implementation counted for each.
3. `core/tools/golden/check_parity.sh` reruns the whole local differentiation — the Kotlin
   dumper and the C++ dumper must produce byte-identical piece segmentation and counts:

   ```sh
   KOTLINC=/path/to/kotlinc/bin ./core/tools/golden/check_parity.sh
   ```

To extend the corpus, edit `core/tools/golden/generate_corpus.py` (seeded, deterministic),
regenerate `corpus.b64`, re-freeze `golden_counts.txt` with the Kotlin dumper, and rerun the
tests — that is the reviewable definition of "the port still agrees".

## Module status

Every **done** row is pinned by golden traces under `core/tools/golden/` and replayed by the
parity tests in `core/tests/` — a drifting port is a failing build, not a surprise on a phone.

| Module | State | Notes |
| --- | --- | --- |
| `tokens/` (BPE tokenizer, counter, memo) | **done** | `O200kBase` prefers `NativeBpeTextTokenCounter` when the library is up; the Kotlin implementation stays as the fallback |
| OpenAI chat pipeline (request factory, SSE consumer, binary codec) | **done** | stands `openai`, `openai-request`; consumed through `NativeOpenAiStream` |
| message preparation (context window, tool rounds, compaction) | **done** | stand `message-preparation` |
| stream normalization (think parsers, termination, retry policy) | **done** | stand `stream-normalization` |
| citations (anchors, dedupe, Anthropic/Gemini projections, JDK `URI`) | **done** | stand `citations` |
| Anthropic provider (stream router + request pipeline) | **done** | stands `anthropic-stream`, `anthropic-request` |
| Gemini provider (stream loop + request pipeline) | **done** | stands `gemini-stream`, `gemini-request` |
| Ollama provider (request pipeline + NDJSON loop) | **done** | stand `ollama` |
| provider support (HTTP error mapping, base-URL versioning, defaults) | **done** | stand `provider-support` |
| web-search scraper + embedding client (parse/encode halves) | **done** | stands `duckduckgo`, `embedding`; transport stays in Kotlin |
| shell tool JSON + bounded file reads | **done** | stand `shell-tools`; UTF-16 unit accounting and the surrogate-safe slice pinned |
| MCP protocol core (tool model, payload parsing, JSON-RPC decisions) | **done** | stand `mcp-protocol`; transport, sessions and the registry stay in Kotlin |
| portable path-glob matcher | **done** | stand `glob-matcher`; mirrors the regex the Kotlin compiles to |
| custom provider name policy (sanitize, dedupe, reserved names) | **done** | stand `provider-names` |
| model alias inference (Claude/Gemini/Nova display aliases) | **done** | stand `model-aliases` |
| custom model references (rename/delete rewriting of id sets, selections, aliases) | **done** | stand `custom-model-references`; compiles verbatim with no stubs, pins `LinkedHashSet`/`LinkedHashMap` order and Kotlin `isBlank`/`trim` |
| run branch selection integrity (durable parent→child projection against the Run tree) | **done** | stand `run-branch-selection`; `RunEntity` lifted verbatim, pins `associateBy` last-wins and null-vs-empty selection keys |
| CJK output guard (Han-ideograph probe, retry instruction) | **done** | stand `cjk-text`; compiles verbatim with no stubs, pins the three BMP ranges and the deliberate exclusion of supplementary-plane Han |
| custom provider identity (stable ids, legacyId, remap) | **done** | stand `provider-identity`; UUID v3 legacy ids (MD5), retry path unit-pinned because it reads entropy |
| memory catalog (active memory, per-file descriptions, metadata store) | **done** | stand `memory-manager`; only `Context`/`StateFlow` are substituted, the store's `.tmp`/`.bak` recovery is pinned |
| bounded semantic embedding selection (keyset scoring, retention heap) | **done** | stand `embedding-selector`; ties break on row id, malformed rows and non-finite scores are counted not fatal |
| Conch shell SSE parser (strict one-event/one-data, shared byte budget) | **done** | stand `conch-sse`; every fail-closed message pinned, `shellUtf8Prefix` extracted verbatim from `ShellClient.kt` |
| native backup media policy (draft rewrite/restore, attachment meta, tool images) | **done** | stand `backup-media-policy`; the call log pins `firstNotNullOfOrNull` probe order, `MessageSegment` lifted verbatim from `ChatMessage.kt` |
| cron expression (5-field grammar, day/hour/minute walk, DST resolution) | **done** | stand `cron-expression`; compiles verbatim with no stubs at all, `tz.cpp` reads TZif and reproduces `Calendar`'s gap/overlap rules |
| task schedule (cron mapping, editor round-trip, one-shot instants) | **done** | stand `task-schedule`; lenient calendar arithmetic pinned, clock-reading paths documented |
| loop policy (cadence/cycle validation, prompt normalization, overflow-safe fire times) | **done** | stand `loop-policy`; compiles verbatim with no stubs at all, pins Kotlin's wrapping `Long` guard |
| conversation runtime reducer (recovery, slot, send/guidance, provider/tool gates, stop/finalization barriers) | **done** | stand `conversation-runtime`; the whole reducer cluster compiles verbatim with no stubs, every state/effect field pinned through Kotlin `toString()` |
| thinking capability (effort normalization, provider family, per-model budgets, resolution) | **done** | stand `thinking-capability`; all four files compile verbatim with no stubs, pins the nearest-effort upward tie rule and the forced-thinking-off fallback |
| token pricing (heuristic/local counting, image rules per family, envelope costs, safety margin) | **done** | stand `token-pricing`; 154 traces drive the shipped pricing rules (no duplicate copy — the rules live in `message_preparation_tokens.cpp`, whose API this stand forced open), `LocalModelTextCounter` ported as the one genuinely missing piece |
| streaming tool arguments (prefix JSON parser, compact tool-card hints, subject normalization) | **done** | stand `streaming-json-document`; both sources compile verbatim (zero imports), `ToolKind` lifted by script; 133 traces pin UTF-16 error offsets, open leaves, every hint branch and the 4096-unit prefix boundary |
| OpenAI service tiers (normalize/aliases, per-model availability, fallback mapping, settings/request value) | **done** | stand `openai-service-tiers`; source compiles verbatim (zero imports); 92 traces pin the model tables, the flex/fast/ultrafast fallbacks and the enabled×responses gate |
| context budget (legacy-window migration, min/max clamp, compact occupancy labels) | **done** | stand `context-budget`; source compiles verbatim (zero imports); 45 traces pin the PRESETS array, the ≤100 migration edge, the clamp and the 1000/1024 label boundaries with Long rounding |
| message source (automation task/loop/ask_user record, canonical stored JSON, backup import gate) | **done** | stand `message-source`; source compiles verbatim (kotlinx codec config), `Participant` lifted by script; 86 traces pin decode degradation, canonical encoding, sanitize gating and the ask_user text form |
| developer-mode unlock (tap counter clamp, seven-tap unlock, remaining-taps feedback) | **done** | stand `developer-mode-unlock`; source compiles verbatim (zero imports); 16 traces pin the counter coercion, the success reset and the three-remaining-taps feedback boundary |
| database compatibility (missing/supported/future/unreadable gate before Room opens, fail-closed canOpen) | **done** | stand `database-compatibility`; source compiles verbatim (zero imports); 23 traces pin the missing short-circuit, every version edge and all pinned JDK exception descriptions |
| V18 regeneration-edge repair (rejected v17→v18 migration forward fix, canonical-user and repaired-root guards, LinkedHashMap ordering) | **done** | stand `v18-regeneration-repair`; planner and real `Constants.kt` compile verbatim, `Participant` lifted by script; 26 traces pin every guard, the sibling ordering and the last-wins lookup map |
| regeneration tree repair (v17 Regenerate/Edit sibling grouping, cloned-input deletion, root reparenting, selection migration, SHA-256 input fingerprint) | **done** | stand `regeneration-tree-repair`; planner compiles verbatim, `Participant` lifted by script; 53 traces pin grouping, anchor ordering, selection fallbacks, dedup contracts and the attachment sanitizer |
| legacy Run backfill (v16 message forest → v17 Runs, deterministic UUID v3 ids, topology order, ambiguity and status derivation, selection migration) | **done** | stand `legacy-run-backfill`; planner compiles verbatim, four model enums lifted by script; 42 traces pin ids, ordering, cycle/orphan guards, statuses and selections |
| data layer (export/import, backups) | pending | |
| tool execution (remaining providers, shell engines) | pending | |

## Rules for this directory

- No third-party dependencies in `core/` itself; the port must keep building with a bare C++20
  toolchain.
- Numeric behaviour is copied, not reinterpreted: same rounding, same clamping, same fallbacks,
  same "skip malformed" rules. When in doubt, read the Kotlin file before writing C++.
- `src/unicode_tables.generated.hpp` is generated — regenerate with
  `javac -d /tmp core/tools/UnicodeTableGen.java && java -cp /tmp UnicodeTableGen core/src/unicode_tables.generated.hpp`
  rather than editing by hand.
- New module = headers in `include/agora/core/`, sources in `src/`, tests that compare against
  the Kotlin original on real data.
