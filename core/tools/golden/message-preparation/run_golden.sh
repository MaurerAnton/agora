#!/usr/bin/env bash
# Generates or verifies the golden traces for the provider-neutral message-preparation pipeline.
#
# The traces are produced by the REAL Kotlin preparation sources compiled straight from the app
# tree (see SOURCES below); leaf stubs replace the Android-only vocabulary loader, the embedded
# runtime and compile-only annotations. The C++ port must reproduce them byte for byte
# (core/tests/message_preparation_parity_test.cpp).
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
vocab="${AGORA_PREP_VOCAB:-$root/app/src/main/assets/tokenizers/o200k_base.tiktoken}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin (set KOTLINX_SERIALIZATION_PLUGIN)" >&2
    exit 2
fi
if [[ ! -f "$vocab" ]]; then
    echo "error: o200k vocabulary not found at $vocab (set AGORA_PREP_VOCAB)" >&2
    exit 2
fi

sources=(
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/MessageConverter.kt"
    "$app/api/util/Base64FileRegistry.kt"
    "$app/api/util/ToolMessages.kt"
    "$app/api/util/ApiMessagePreparation.kt"
    "$app/api/util/ContextWindow.kt"
    "$app/api/util/ContextLimiter.kt"
    "$app/api/util/ContextTokenEstimator.kt"
    "$app/api/util/tokens/TextTokenCounter.kt"
    "$app/api/util/tokens/ImageTokenCost.kt"
    "$app/api/util/tokens/PixelImageTokenCost.kt"
    "$app/api/util/tokens/ContextCostModel.kt"
    "$app/api/util/tokens/ContextCostModels.kt"
    "$app/api/util/tokens/FamilyEnvelopeCosts.kt"
    "$app/api/util/tokens/AnthropicModelId.kt"
    "$app/api/util/tokens/AnthropicImageCost.kt"
    "$app/api/util/tokens/GeminiImageCost.kt"
    "$app/api/util/tokens/OpenAiImageCost.kt"
    "$app/api/util/tokens/BpeTextTokenCounter.kt"
    "$app/api/util/tokens/LocalModelTextCounter.kt"
    "$app/api/util/tokens/CostModelContextEstimator.kt"
    "$app/api/util/tokens/bpe/BpeVocabulary.kt"
    "$app/api/util/tokens/bpe/BpeTokenCount.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/util/Constants.kt"
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/stubs/com/newoether/agora/api/LocalModelRuntime.kt"
    "$here/stubs/com/newoether/agora/api/util/tokens/bpe/O200kBase.kt"
    "$here/MessagePreparationTraceDump.kt"
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
[[ -f "$classes/message/preparation/golden/MessagePreparationTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

runner="$kotlinc_dir/kotlin"
for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    "$runner" -cp "$classes:$jars" message.preparation.golden.MessagePreparationTraceDumpKt "$scenario" "$vocab" > "$fresh/$name.trace"
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
