#!/usr/bin/env bash
# Generates or verifies the golden traces for api/openai/OpenAiRequestValidator.kt.
#
# The validator, the wire-name grammars, the request-format gate and the request models compile
# VERBATIM from the app tree; the four Responses model declarations are lifted byte-for-byte out of
# api/LlmProvider.kt by extract_responses_models.py (that file is the provider abstraction and
# pulls in the message-preparation and cost-model clusters).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS must point at the kotlinx jars" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
models="$here/build/ResponsesModels.kt"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the Responses request models from LlmProvider.kt..."
python3 "$here/extract_responses_models.py" "$app/api/LlmProvider.kt" "$models"

sources=(
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/ModelId.kt"
    "$app/util/Constants.kt"
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/RequestFormatValidation.kt"
    "$app/api/openai/OpenAiWireValidationJson.kt"
    "$app/api/openai/OpenAiRequestValidator.kt"
    "$models"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/OpenAiResponsesRequestTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/openai/responses/request/golden/OpenAiResponsesRequestTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     openai.responses.request.golden.OpenAiResponsesRequestTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json

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
