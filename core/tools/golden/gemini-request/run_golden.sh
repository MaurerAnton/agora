#!/usr/bin/env bash
# Generates or verifies the golden traces for the Gemini request pipeline.
#
# The traces are produced by the REAL Kotlin declarations: the wire models and the validator are
# compiled from the app sources, and the request-side provider helpers are extracted verbatim by
# `agora_extract gemini-request`. The C++ port must reproduce them byte for byte
# (core/tests/gemini_request_parity_test.cpp).
#
# Requirements: identical to core/tools/golden/openai-request/run_golden.sh (kotlinc with the
# serialization compiler plugin, kotlinx-serialization jars via KOTLINX_JARS).
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

# Sources that must exist in the app tree (checked upfront)
app_sources=(
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/RequestFormatValidation.kt"
    "$app/api/util/MessageConverter.kt"
    "$app/api/util/Base64FileRegistry.kt"
    "$app/api/util/ProviderThinkingResolution.kt"
    "$app/api/util/ToolMessages.kt"
    "$app/api/util/ToolSchemaJson.kt"
    "$app/model/ModelThinkingCapabilities.kt"
    "$app/model/ModelThinkingCapability.kt"
    "$app/model/ModelThinkingCapabilityDefaults.kt"
    "$app/model/ThinkingControl.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/util/Constants.kt"
    "$app/api/gemini/GeminiWireModels.kt"
    "$app/api/gemini/GeminiRequestValidator.kt"
)

# Sources that live in the stand directory (checked after generation)
stand_sources=(
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/build/GeminiRequestHelpers.kt"
    "$here/GeminiRequestTraceDump.kt"
)

for source in "${app_sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing app source $source" >&2; exit 2; }
done

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$classes" "$fresh"
mkdir -p "$classes" "$fresh"

echo "Extracting the request-side helpers from GeminiProvider.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" gemini-request "$app/api/gemini/GeminiProvider.kt" "$here/build/GeminiRequestHelpers.kt"

for source in "${stand_sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing stand source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference pipeline..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${app_sources[@]}" "${stand_sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/gemini/request/golden/GeminiRequestTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         gemini.request.golden.GeminiRequestTraceDumpKt "$scenario" "$here" > "$fresh/$name.trace"
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