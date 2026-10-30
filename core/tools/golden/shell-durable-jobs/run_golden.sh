#!/usr/bin/env bash
# Generates or verifies the golden traces for ShellDurableJobExecutor.kt.
#
# ShellDurableJobExecutor.kt and ShellToolJson.kt compile VERBATIM from the app tree. The only
# substitutes are the stand-local leaf doubles in platform/:
#  1. GenerationContext / ShellDeviceConfig / ToolCallData — the exact fields the executor reads;
#  2. ConchBackend — a scripted double that replaces the OkHttp transport, records every backend
#     event (start/get/list/stop/close) and can cancel the calling coroutine on a "cancel" step.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx jars)" >&2
    exit 2
fi

# The executor suspends: locate the coroutines jar next to the serialization jars by default.
coroutines="${KOTLINX_COROUTINES_JAR:-}"
if [[ -z "$coroutines" ]]; then
    first="${jars%%:*}"
    coroutines="$(ls "$(dirname "$first")"/kotlinx-coroutines-core-jvm-*.jar 2>/dev/null | head -1 || true)"
fi
if [[ ! -f "$coroutines" ]]; then
    echo "error: kotlinx-coroutines-core-jvm jar not found (set KOTLINX_COROUTINES_JAR)" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$coroutines"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" shell-durable-jobs "$here"

sources=(
    "$app/tool/ShellDurableJobExecutor.kt"
    "$app/tool/ShellToolJson.kt"
    "$here/platform/GenerationContext.kt"
    "$here/platform/ShellDeviceConfig.kt"
    "$here/platform/ToolCallData.kt"
    "$here/platform/ConchBackend.kt"
    "$here/ShellDurableJobTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars:$coroutines" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/durable/jobs/golden/ShellDurableJobTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     shell.durable.jobs.golden.ShellDurableJobTraceDumpKt \
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
