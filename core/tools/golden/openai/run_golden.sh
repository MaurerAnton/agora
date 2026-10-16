#!/usr/bin/env bash
# Generates or verifies the golden traces for the OpenAI chat SSE consumer.
#
# The traces are produced by the REAL Kotlin consumer compiled straight from the app sources
# (see SOURCES below) plus two leaf stubs for Android-only types. The C++ port must reproduce
# them byte for byte (core/tests/openai_stream_test.cpp).
#
# Requirements:
#   - kotlinc (KOTLINC env or on PATH), any 2.x distribution
#   - kotlinx-serialization jars matching the app version (gradle/libs.versions.toml), paths in
#     KOTLINX_JARS (colon-separated kotlinx-serialization-json-jvm.jar and
#     kotlinx-serialization-core-jvm.jar)
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify transcripts against the frozen traces
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
    "$app/api/StreamEvent.kt"
    "$app/api/OpenAiStreamModels.kt"
    "$app/api/GenerationError.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/ToolArgumentAccumulator.kt"
    "$app/api/util/MalformedToolCall.kt"
    "$app/api/util/ProviderRetryPolicy.kt"
    "$app/api/util/StreamTermination.kt"
    "$app/api/openai/OpenAiChatStreamConsumer.kt"
    "$app/model/TokenUsage.kt"
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/newoether/agora/model/CitationRecord.kt"
    "$here/OpenAiStreamTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$classes" "$fresh"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference consumer..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/openai/golden/OpenAiStreamTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

runner="$kotlinc_dir/kotlin"
for transcript in "$here"/transcripts/*.txt; do
    name="$(basename "$transcript" .txt)"
    "$runner" -cp "$classes:$jars" openai.golden.OpenAiStreamTraceDumpKt "$transcript" > "$fresh/$name.trace"
done

count="$(ls "$fresh"/*.trace | wc -l)"

if [[ "${1:-}" == "--freeze" ]]; then
    rm -f "$here"/golden/*.trace
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
