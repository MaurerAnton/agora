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
| custom provider identity (stable ids, legacyId, remap) | **done** | stand `provider-identity` |
| memory catalog (active memory, per-file descriptions, metadata store) | **done** | stand `memory-manager`; only `Context`/`StateFlow` are substituted, the store's `.tmp`/`.bak` recovery is pinned |
| bounded semantic embedding selection (keyset scoring, retention heap) | **done** | stand `embedding-selector`; ties break on row id, malformed rows and non-finite scores are counted not fatal |
| Conch shell SSE parser (strict one-event/one-data, shared byte budget) | **done** | stand `conch-sse`; every fail-closed message pinned, `shellUtf8Prefix` extracted verbatim from `ShellClient.kt` |
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
