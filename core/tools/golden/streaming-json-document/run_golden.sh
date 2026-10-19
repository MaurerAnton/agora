#!/usr/bin/env bash
# Generates or verifies the golden traces for the streaming-json-document cluster.
#
# ui/chat/message/StreamingJsonDocument.kt and ui/chat/message/StreamingToolArgumentHints.kt
# compile VERBATIM from the app tree (both have zero imports). ToolKind comes from
# ToolPresentation.kt, lifted declaration-for-declaration by extract_tool_kind.py into
# extracted/ToolKind.kt. The C++ core must reproduce the traces byte for byte
# (core/tests/streaming_json_parity_test.cpp).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
src="$root/app/src/main/java/com/newoether/agora/ui/chat/message"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi

python3 "$here/extract_tool_kind.py"

# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$src/StreamingJsonDocument.kt" \
    "$src/StreamingToolArgumentHints.kt" \
    "$here/extracted/ToolKind.kt" \
    "$here/StreamingJsonTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/streaming/json/golden/StreamingJsonTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         streaming.json.golden.StreamingJsonTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
