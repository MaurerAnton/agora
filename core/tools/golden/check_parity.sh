#!/usr/bin/env bash
# Runs the full parity differentiation: compiles the Kotlin dumper from the app sources, builds
# the C++ dumper, and requires byte-identical piece segmentation and token counts on the whole
# corpus. Needs a C++20 compiler, CMake, Java and kotlinc (KOTLINC=... or on PATH).
set -euo pipefail

cd "$(dirname "$0")/../../.."

KOTLINC="${KOTLINC:-kotlinc}"
VOCAB="app/src/main/assets/tokenizers/o200k_base.tiktoken"
CORPUS="core/tools/golden/corpus.b64"

WORK="$(mktemp -d)"
trap 'rm -rf "$WORK"' EXIT

echo "== compiling the Kotlin dumper =="
"$KOTLINC" \
    app/src/main/java/com/newoether/agora/api/util/tokens/bpe/BpeVocabulary.kt \
    app/src/main/java/com/newoether/agora/api/util/tokens/bpe/BpeTokenCount.kt \
    core/tools/golden/GoldenDump.kt \
    -include-runtime -d "$WORK/golden.jar" >/dev/null

echo "== building the C++ dumper =="
cmake -S core -B "$WORK/build" -DCMAKE_BUILD_TYPE=Release >/dev/null
cmake --build "$WORK/build" --target agora_core_dump >/dev/null

status=0
for mode in pieces count; do
    java -jar "$WORK/golden.jar" "$VOCAB" "$mode" "$CORPUS" > "$WORK/kotlin.$mode"
    "$WORK/build/agora_core_dump" "$VOCAB" "$mode" "$CORPUS" > "$WORK/cpp.$mode"
    if diff -q "$WORK/kotlin.$mode" "$WORK/cpp.$mode" >/dev/null; then
        echo "parity holds: $mode, $(wc -l < "$CORPUS") cases"
    else
        echo "PARITY BROKEN: $mode; first differences:" >&2
        diff "$WORK/kotlin.$mode" "$WORK/cpp.$mode" | head -6 >&2
        status=1
    fi
done

if [ "$status" -eq 0 ] && [ "${1:-}" = "--freeze" ]; then
    java -jar "$WORK/golden.jar" "$VOCAB" count "$CORPUS" > core/tools/golden/golden_counts.txt
    echo "frozen: core/tools/golden/golden_counts.txt"
fi
exit "$status"
