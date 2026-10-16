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

| Module | State | Notes |
| --- | --- | --- |
| `tokens/` (BPE tokenizer, counter, memo) | **done** | `O200kBase` prefers `NativeBpeTextTokenCounter` when the library is up; the Kotlin implementation stays as the fallback |
| provider protocols, HTTP/SSE streaming | pending | next; needs a JSON and stream layer first |
| data layer (memory, stored files) | pending | |
| tool execution, MCP client | pending | |

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
