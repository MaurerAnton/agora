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
| run recovery (process-death status reset, incomplete tool stop, raw segment JSON rewrite) | **done** | stand `run-recovery`; policy compiles verbatim, six model declarations lifted byte-for-byte from `ChatMessage.kt`, only the Compose `@Immutable` marker is stand-local; 54 traces pin every field through data-class `toString()` and exact raw/compact JSON outcomes |
| export spool (newline-delimited spool writer, layout index, streaming slice reader) | **done** | stand `export-spool`; `ExportSpool.kt` compiles verbatim with no platform substitute at all (pure `java.io`); 31 traces pin the `TYPE\tpayload\n` frame, the flush-point byte offsets (UTF-8 math included), the conversation/task ranges, the writer state machine with its `IllegalStateException` message, the reader clamping/malformed-record rules and the iterator chunk/slice carry with its exhausted state |
| CJK output guard (Han-ideograph probe, retry instruction) | **done** | stand `cjk-text`; compiles verbatim with no stubs, pins the three BMP ranges and the deliberate exclusion of supplementary-plane Han |
| custom provider identity (stable ids, legacyId, remap) | **done** | stand `provider-identity`; UUID v3 legacy ids (MD5), retry path unit-pinned because it reads entropy |
| memory catalog (active memory, per-file descriptions, metadata store) | **done** | stand `memory-manager`; only `Context`/`StateFlow` are substituted, the store's `.tmp`/`.bak` recovery is pinned |
| bounded semantic embedding selection (keyset scoring, retention heap) | **done** | stand `embedding-selector`; ties break on row id, malformed rows and non-finite scores are counted not fatal |
| Conch shell SSE parser (strict one-event/one-data, shared byte budget) | **done** | stand `conch-sse`; every fail-closed message pinned, `shellUtf8Prefix` extracted verbatim from `ShellClient.kt` |
| native backup media policy (draft rewrite/restore, attachment meta, tool images) | **done** | stand `backup-media-policy`; the call log pins `firstNotNullOfOrNull` probe order, `MessageSegment` lifted verbatim from `ChatMessage.kt` |
| message persistence guard (text cap, bounded segment JSON, largest-field trimming, Responses-state protection) | **done** | stand `message-persistence-guard`; `MessagePersistenceGuard.kt` and `Constants.kt` compile verbatim, `MessageSegment` lifted by the backup stand's extractor; 65 traces pin UTF-16 boundaries, escape costs, tie order, floor/termination, the 600000-byte default and a deterministic mixed-field corpus |
| imported citation projection (ChatGPT/Claude marker stripping, reference walking, anchor resolution, bounded citation-row encode) | **done** | stand `imported-citation-projection`; projection, `Citation.kt`, `MessagePersistenceGuard.kt` and `ChatMessage.kt` compile verbatim with the citations stand's leaf stubs — CitationPolicy/guard/MessageSegment are reused, never reimplemented; 51 traces pin both private-marker regexes over UTF-16, firstNotNullOfOrNull order, structured ref ids, the three anchor branches with the `intOrNull` JSON grammar, the Claude block-offset override and the 600000-byte trim of citation segments |
| native backup secrets policy (capture/restore API keys, shell, embedding and MCP credentials, replace/merge, orphan warnings) | **done** | stand `native-backup-secrets`; policy and shared settings models compile verbatim, DataStore `SettingsManager` replaced by an in-memory storage double; 9 traces pin map order, id remapping, semantic key matching, clearing and warning order |
| native backup format (version gate, entry vocabulary, conversation entry hashing, credential stripping) | **done** | stand `native-backup-format`; `NativeBackupFormat.kt` compiles verbatim; 5 traces pin constants, supported-version edges, UTF-8 hex entries and model projections |
| native Run archive policy (terminalize imported rows, ownership completeness, parent-before-child ordering) | **done** | stand `native-run-archive`; policy, `RuntimeIdentity.kt` and `RunEntity.kt` compile verbatim, only Room annotations and the `ChatEntity` table are stubbed; 32 traces pin every status/end-reason fallback, timestamp and pass clamps, ownership uniqueness and missing-parent/cycle/duplicate-id ordering |
| portable settings archive (settings.json export allowlist, restore merges, provider reference remap, secrets gating, font/prompt resolution) | **done** | stand `portable-settings-archive`; `PortableSettingsArchive.kt` and its settings/provider models compile verbatim, the identity policy lifted by the `provider-identity` extractor, DataStore `SettingsManager` replaced by a recording double over the full 97-flow/97-save surface; 110 traces pin the exact kotlinx export text (`encodeDefaults=false`), accessor/decode coercions, merge order, warnings, guard messages and generated-vs-retained provider ids |
| portable settings reset (the portable allowlist a settings import clears) | **done** | stand `portable-settings-reset`; `PortableSettingsReset.kt` and `SettingsPreferenceSchema.kt` compile verbatim, only the DataStore preferences contract is an in-memory stub; the C++ key list is **generated from the Kotlin source**, so the port cannot drift — the trace pins the 94 removed keys, the 37 that survive (secrets, device-local state, sandbox, lifecycle and auto-backup configuration) and the rewritten provider-name map |
| legacy extra settings (backup v<4 JSON reader: scalar fields, conversation records, aliases, custom models, secret gate) | **done** | stand `export-extra-settings`; `ExportExtraSettings.kt`, `ConversationSettings` and the thinking/tier policies compile verbatim, DataStore `SettingsManager` replaced by a recording storage double; 164 traces pin the exact save-call order and arguments, kotlinx primitive spellings, merge/replace order and partial writes before a typed failure |
| cron expression (5-field grammar, day/hour/minute walk, DST resolution) | **done** | stand `cron-expression`; compiles verbatim with no stubs at all, `tz.cpp` reads TZif and reproduces `Calendar`'s gap/overlap rules |
| task schedule (cron mapping, editor round-trip, one-shot instants) | **done** | stand `task-schedule`; lenient calendar arithmetic pinned, clock-reading paths documented |
| loop policy (cadence/cycle validation, prompt normalization, overflow-safe fire times) | **done** | stand `loop-policy`; compiles verbatim with no stubs at all, pins Kotlin's wrapping `Long` guard |
| ZIP reader (local file headers, STORED/DEFLATED entries, data descriptors, the JDK error contract) | **done** | stand `zip-reader`; the reference is the JDK's `java.util.zip.ZipInputStream` (archives built by `ZipOutputStream`, frozen as fixtures) and the C++ port ships a from-scratch RFC 1951 DEFLATE decoder — no third-party dependency in core; 39 traces pin the entry list, both compression methods, data descriptors and every JDK failure message, including zlib's tree-validity wording |
| Claude chat import (export decoding, preview, chat/message entities, attachments, citations) | **done** | stand `claude-chat-importer`; `ClaudeChatImporter.kt`, `ImportStreams.kt` and the citation/attachment models compile verbatim (the citation projection and the attachment encoder are reused, not reimplemented); 31 traces pin the ZIP entry selection, kotlinx's lenient+coerce decode rules, the preview ordering and title fallbacks, the content-block merge, the citation offsets in UTF-16 units, the attachment metadata JSON and the clock fallback for an unparsable timestamp |
| GPT chat import (conversation graph rebuild, preview, ephemeral-node merge, branch map) | **done** | stand `gpt-chat-importer`; `GptChatImporter.kt`, `ClaudeChatImporter.kt` (the shared import entities) and the citation/attachment models compile verbatim — the citation projection and the ZIP reader are reused, not reimplemented; 34 traces pin the validity pass over the mapping (cycles and dangling parents), the PriorityQueue graph order, the current-node path, the ephemeral-ancestor merge, the parent cascade, the selected-branches JSON, the `decode` category and the clock fallback |
| conversation runtime reducer (recovery, slot, send/guidance, provider/tool gates, stop/finalization barriers) | **done** | stand `conversation-runtime`; the whole reducer cluster compiles verbatim with no stubs, every state/effect field pinned through Kotlin `toString()` |
| thinking capability (effort normalization, provider family, per-model budgets, resolution) | **done** | stand `thinking-capability`; all four files compile verbatim with no stubs, pins the nearest-effort upward tie rule and the forced-thinking-off fallback |
| model thinking resolver (selected-model id parse, custom-provider identity/protocol, capability lookup) | **done** | stand `model-thinking-resolver`; resolver, `ModelId`, `CustomProviderConfig` and capability cluster compile verbatim, only the `isStableId` slice is extracted from `CustomProviderIdentity.kt`; 36 traces pin every id heuristic, name/id/legacy match and protocol family |
| provider protocol support (protocol detection, prompt cache switches, Anthropic TTL, Responses API availability) | **done** | stand `provider-protocol-support`; `CustomProviderConfig.kt` compiles verbatim, same documented `isStableId` extraction; 28 traces pin any-match vs first-match, built-in shadowing, cache/TTL/Responses fallbacks |
| token pricing (heuristic/local counting, image rules per family, envelope costs, safety margin) | **done** | stand `token-pricing`; 154 traces drive the shipped pricing rules (no duplicate copy — the rules live in `message_preparation_tokens.cpp`, whose API this stand forced open), `LocalModelTextCounter` ported as the one genuinely missing piece |
| streaming tool arguments (prefix JSON parser, compact tool-card hints, subject normalization) | **done** | stand `streaming-json-document`; both sources compile verbatim (zero imports), `ToolKind` lifted by script; 133 traces pin UTF-16 error offsets, open leaves, every hint branch and the 4096-unit prefix boundary |
| OpenAI service tiers (normalize/aliases, per-model availability, fallback mapping, settings/request value) | **done** | stand `openai-service-tiers`; source compiles verbatim (zero imports); 92 traces pin the model tables, the flex/fast/ultrafast fallbacks and the enabled×responses gate |
| OpenAI Responses request (chat→Responses input builder, reasoning-item replay, tool conversion, wire-format gate) | **done** | stand `openai-responses-request`; the validator, the wire-name grammars and the request-format gate compile verbatim, the four Responses request models are lifted byte-for-byte out of `LlmProvider.kt` by the stand's extractor; 54 traces pin the input items, the provider+model replay rule, the tool conversion and every violation of the local gate (with the provider-style message and the distinct violation list) |
| Run UI projection (message generation boundaries, edit/regenerate branch affordances) | **done** | stand `run-ui-projection`; `MessageGenerationBoundary.kt` and `RunUiProjection.kt` compile verbatim with `ChatMessage.kt` (the already-ported `message_preparation` model is reused); 26 traces pin the boundary grouping (real USER rows, durable Run ids, protocol/compact rows), the id-identity rule, the two independent branch dimensions with their sibling order, the delete target of a regenerated branch and the structural-input fallback |
| context budget (legacy-window migration, min/max clamp, compact occupancy labels) | **done** | stand `context-budget`; source compiles verbatim (zero imports); 45 traces pin the PRESETS array, the ≤100 migration edge, the clamp and the 1000/1024 label boundaries with Long rounding |
| message source (automation task/loop/ask_user record, canonical stored JSON, backup import gate) | **done** | stand `message-source`; source compiles verbatim (kotlinx codec config), `Participant` lifted by script; 86 traces pin decode degradation, canonical encoding, sanitize gating and the ask_user text form |
| developer-mode unlock (tap counter clamp, seven-tap unlock, remaining-taps feedback) | **done** | stand `developer-mode-unlock`; source compiles verbatim (zero imports); 16 traces pin the counter coercion, the success reset and the three-remaining-taps feedback boundary |
| database compatibility (missing/supported/future/unreadable gate before Room opens, fail-closed canOpen) | **done** | stand `database-compatibility`; source compiles verbatim (zero imports); 23 traces pin the missing short-circuit, every version edge and all pinned JDK exception descriptions |
| V18 regeneration-edge repair (rejected v17→v18 migration forward fix, canonical-user and repaired-root guards, LinkedHashMap ordering) | **done** | stand `v18-regeneration-repair`; planner and real `Constants.kt` compile verbatim, `Participant` lifted by script; 26 traces pin every guard, the sibling ordering and the last-wins lookup map |
| regeneration tree repair (v17 Regenerate/Edit sibling grouping, cloned-input deletion, root reparenting, selection migration, SHA-256 input fingerprint) | **done** | stand `regeneration-tree-repair`; planner compiles verbatim, `Participant` lifted by script; 53 traces pin grouping, anchor ordering, selection fallbacks, dedup contracts and the attachment sanitizer |
| legacy Run backfill (v16 message forest → v17 Runs, deterministic UUID v3 ids, topology order, ambiguity and status derivation, selection migration) | **done** | stand `legacy-run-backfill`; planner compiles verbatim, four model enums lifted by script; 42 traces pin ids, ordering, cycle/orphan guards, statuses and selections |
| v16→v17 migration (SQL schema upgrade, planned Run inserts, message assignment, selection JSON migration, unassigned-message guard) | **done** | stand `migration-16-17`; migration and planner compile verbatim, only Room/SQLite platform types shimmed; 21 traces pin every statement, bind-argument type, failure prefix and selection decode |
| v17→v18 migration (Regenerate-as-Edit repair SQL, Run reparenting, cloned-input deletion, sequence rewrite, selection migration) | **done** | stand `migration-17-18`; migration and tree planner compile verbatim, Room/SQLite/Cursor platform types shimmed; 24 traces pin projections, repair order, fingerprint grouping, image-count paths and selection outcomes |
| v18→v19 migration (forward repair for the rejected v18 build, re-runs the v18 regeneration-edge repair, ordered parent updates) | **done** | stand `migration-18-19`; migration and v18 planner compile verbatim, Room/SQLite/Cursor platform types shimmed, `Participant` lifted by script; 21 traces pin both projections, the ambiguous gate, canonical selection and update order |
| v19→v20 migration (repairs legacy oversized message rows without reading them through a cursor, clears segment JSON and clips text/thoughts) | **done** | stand `migration-19-20`; migration and real `Constants.kt` compile verbatim, `MessagePersistenceGuard.TRUNCATION_MARKER` lifted by script; 1 trace pins all three statements, their order, whitespace and Int/String bind types |
| v20→v35 schema migrations (token usage columns, unread generation flag, model/attachment indexes, New Chat persist and settings outbox tables, maintenance debt, semantic index ledger/work, duration/dataChangedAt/prompt/source columns) | **done** | stand `migration-20-35`; all fifteen migrations compile verbatim, only Room/SQLite platform types shimmed; 15 traces pin every statement, order and whitespace, with no bind arguments |
| data layer (export/import, backups) | in progress | backup media, secrets, format helpers, the portable settings archive, the legacy extra-settings reader and the export spool are pinned; ZIP archive writers/readers and import/export orchestration remain |
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
