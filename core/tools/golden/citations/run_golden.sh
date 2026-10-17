#!/usr/bin/env bash
# Generates or verifies the golden traces for the citation pipeline (CitationPolicy + the
# Anthropic and Gemini citation projections + the java.net.URI surface safeHttpUrl is built on).
#
# The traces are produced by the REAL Kotlin sources compiled straight from the app tree (see
# SOURCES below); leaf stubs replace Android-only loaders and compile-only annotations. The C++
# port must reproduce them byte for byte (core/tests/citations_parity_test.cpp).
#
# Requirements: identical to the other golden stands (kotlinc with the serialization compiler
# plugin, kotlinx-serialization jars via KOTLINX_JARS).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin (set KOTLINX_SERIALIZATION_PLUGIN)" >&2
    exit 2
fi

sources=(
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/Citation.kt"
    "$app/util/Constants.kt"
    "$app/api/anthropic/AnthropicCitationProjection.kt"
    "$app/api/gemini/GeminiCitationProjection.kt"
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/CitationsTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$classes" "$fresh"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference pipeline..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/citations/golden/CitationsTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

runner="$kotlinc_dir/kotlin"
for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    "$runner" -cp "$classes:$jars" citations.golden.CitationsTraceDumpKt "$scenario" > "$fresh/$name.trace"
done

count="$(ls "$fresh"/*.trace | wc -l)"

if [[ "${1:-}" == "--freeze" ]]; then
    rm -f "$here"/golden/*.trace
    mkdir -p "$here/golden"
    cp "$fresh"/*.trace "$here/golden/"
    echo "Frozen $count traces into $here/golden"
    exit 0
fi

if diff -r "$here/golden" "$fresh" > "$here/build/diff.txt"; then
    echo "PARITY OK: $count traces identical to the frozen golden"
else
    echo "GOLDEN DRIFT:" >&2
    head -80 "$here/build/diff.txt" >&2
    exit 1
fi
