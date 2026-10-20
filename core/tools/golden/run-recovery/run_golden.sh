#!/usr/bin/env bash
# Generates or verifies the golden traces for model/RunRecoveryPolicy.kt.
#
# RunRecoveryPolicy.kt compiles VERBATIM from the app tree. It needs the model declarations that
# live in model/ChatMessage.kt (MessageSegment, Participant, MessageStatus, ToolExecutionStates,
# plus MessageSegment's ToolImageAttachment/StreamingTextDelta field types). ChatMessage.kt cannot
# compile host-side (Compose @Immutable, Room, UI state), so extract_model_types.py copies those
# declarations out verbatim instead of retyping them. The Compose marker is the only substituted
# surface, supplied by a stand-local stub. The C++ core must reproduce the traces byte for byte
# (core/tests/run_recovery_parity_test.cpp).
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
# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the model declarations from ChatMessage.kt verbatim..."
python3 "$here/extract_model_types.py" "$app/model/ChatMessage.kt" "$here/build/RunRecoveryModel.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" \
    "$here/build/RunRecoveryModel.kt" \
    "$here/stubs/androidx/compose/runtime/Immutable.kt" \
    "$app/model/RunRecoveryPolicy.kt" \
    "$here/RunRecoveryTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/run/recovery/golden/RunRecoveryTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         run.recovery.golden.RunRecoveryTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
