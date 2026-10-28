#!/usr/bin/env bash
# Generates or verifies the golden traces for model/MessagePersistenceGuard.kt.
#
# MessagePersistenceGuard.kt and util/Constants.kt compile VERBATIM from the app tree.
# MessageSegment / ToolImageAttachment / StreamingTextDelta live in model/ChatMessage.kt, which
# cannot compile host-side, so the backup-media-policy extractor lifts those declarations out
# byte for byte (only the Compose @Immutable marker is stand-local).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify against golden/
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate after changing scenarios
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting MessageSegment / ToolImageAttachment / StreamingTextDelta verbatim..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" message-segment \
    "$app/model/ChatMessage.kt" "$here/build/MessageSegment.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" \
    "$here/build/MessageSegment.kt" \
    "$here/../backup-media-policy/PlatformStubs.kt" \
    "$app/util/Constants.kt" \
    "$app/model/MessagePersistenceGuard.kt" \
    "$here/MessagePersistenceTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/message/guard/golden/MessagePersistenceTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         message.guard.golden.MessagePersistenceTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
