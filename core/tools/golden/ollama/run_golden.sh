#!/usr/bin/env bash
# Generates or verifies the golden traces for the Ollama request pipeline and stream loop.
#
# The traces are produced by the REAL Kotlin sources: the wire models and loop helpers are
# extracted verbatim from OllamaProvider.kt, the validator compiles straight from the app tree,
# and the driver transcribes the provider's local functions. The C++ port must reproduce them
# byte for byte (core/tests/ollama_parity_test.cpp).
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
    "$app/api/OpenAiStreamModels.kt"
    "$app/api/StreamEvent.kt"
    "$app/api/GenerationError.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/ProviderRetryPolicy.kt"
    "$app/api/util/RequestFormatValidation.kt"
    "$app/api/util/MessageConverter.kt"
    "$app/api/util/Base64FileRegistry.kt"
    "$app/api/util/ProviderThinkingResolution.kt"
    "$app/api/util/ToolMessages.kt"
    "$app/api/util/ToolSchemaJson.kt"
    "$app/api/util/StreamTermination.kt"
    "$app/api/util/MalformedToolCall.kt"
    "$app/api/util/ToolArgumentAccumulator.kt"
    "$app/api/ollama/OllamaRequestValidator.kt"
    "$app/model/ModelThinkingCapabilities.kt"
    "$app/model/ModelThinkingCapability.kt"
    "$app/model/ModelThinkingCapabilityDefaults.kt"
    "$app/model/ThinkingControl.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/Citation.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/util/Constants.kt"
)

# Sources that live in the stand directory (checked after generation)
stand_sources=(
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/build/OllamaModels.kt"
    "$here/OllamaTraceDump.kt"
)

for source in "${app_sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing app source $source" >&2; exit 2; }
done

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$classes" "$fresh"
mkdir -p "$classes" "$fresh" "$here/build"

echo "Extracting the wire models and loop helpers from OllamaProvider.kt..."
python3 "$here/extract_ollama_models.py" "$app/api/ollama/OllamaProvider.kt" "$here/build/OllamaModels.kt"

for source in "${stand_sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing stand source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference pipeline..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${app_sources[@]}" "${stand_sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/ollama/golden/OllamaTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         ollama.golden.OllamaTraceDumpKt "$scenario" "$here" > "$fresh/$name.trace"
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