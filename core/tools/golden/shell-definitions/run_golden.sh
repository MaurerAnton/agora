#!/usr/bin/env bash
# Generates or verifies the golden traces for the shell tool definition builder.
#
# ShellToolDefinitions.kt compiles VERBATIM from the app tree against two stand-only leaf stubs
# (GenerationContext, ShellDeviceConfig) and the real maxWaitMs helper extracted from
# ShellDurableJobExecutor.kt; the driver calls build() directly. The C++ port must reproduce the
# traces byte for byte (core/tests/shell_definitions_parity_test.cpp).
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

classes="$here/build/classes"
fresh="$here/build/golden"
wait_job="$here/build/WaitJob.kt"
rm -rf "$classes" "$fresh"
mkdir -p "$classes" "$fresh" "$here/build"

echo "Extracting maxWaitMs from ShellDurableJobExecutor.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" wait-job "$app/tool/ShellDurableJobExecutor.kt" "$wait_job"

sources=(
    "$app/api/ProviderRequestModels.kt"
    "$app/api/ToolParametersSerializer.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/ChatMessage.kt"
    "$app/util/Constants.kt"
    "$here/stubs/androidx/compose/runtime/Immutable.kt"
    "$here/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/stubs/com/newoether/agora/viewmodel/GenerationContext.kt"
    "$here/stubs/com/newoether/agora/data/ShellDeviceConfig.kt"
    "$wait_job"
    "$app/tool/ShellToolDefinitions.kt"
    "$here/ShellDefinitionsTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/definitions/golden/ShellDefinitionsTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2; exit 2; }

for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         shell.definitions.golden.ShellDefinitionsTraceDumpKt "$scenario" "$here" > "$fresh/$name.trace"
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