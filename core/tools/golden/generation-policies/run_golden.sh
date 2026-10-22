#!/usr/bin/env bash
# Generates or verifies the golden traces for viewmodel/GenerationPolicies.kt and
# viewmodel/AutomaticMessageApiProjection.kt.
#
# The two policy files are lifted verbatim by extract_policies.py, which only widens `internal` to
# the default visibility so the driver can call them; ChatMessage.kt, MessageSource.kt,
# PromptTemplateItem.kt (PredefinedVariables) and Constants.kt compile VERBATIM from the app tree.
#
# The JVM runs with a fixed time zone (UTC) so the templated timestamps are deterministic.
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
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the generation policies..."
python3 "$here/extract_policies.py"

sources=(
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/data/PromptTemplateItem.kt"
    "$app/model/RuntimeIdentity.kt"
    "$app/api/util/ToolMessages.kt"
    "$app/api/util/SafeWireNames.kt"
    "$app/api/util/MessageConverter.kt"
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/api/util/Base64FileRegistry.kt"
    "$app/util/Constants.kt"
    "$here/build/GenerationPolicies.kt"
    "$here/build/AutomaticMessageApiProjection.kt"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/GenerationPoliciesTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/generation/policies/golden/GenerationPoliciesTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -Duser.timezone=UTC -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     generation.policies.golden.GenerationPoliciesTraceDumpKt \
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
