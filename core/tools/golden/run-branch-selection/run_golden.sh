#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/RunBranchSelectionIntegrity.kt.
#
# RunBranchSelectionIntegrity.kt compiles VERBATIM from the app tree. It is declared over RunEntity,
# so the stand needs that type: agora_extract run-entity lifts the data class out of RunEntity.kt
# verbatim (that file's Room catalogue entry cannot compile host-side) and PlatformStubs.kt
# supplies the single Room marker. model/RuntimeIdentity.kt is compiled verbatim for RunStatus and
# RunEndReason. The C++ core must reproduce the traces byte for byte
# (core/tests/run_branch_selection_parity_test.cpp).
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
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi

# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting RunEntity verbatim from RunEntity.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" run-entity "$app/data/local/RunEntity.kt" "$here/build/RunEntity.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$here/build/RunEntity.kt" \
    "$here/PlatformStubs.kt" \
    "$app/model/RuntimeIdentity.kt" \
    "$app/data/local/RunBranchSelectionIntegrity.kt" \
    "$here/RunBranchSelectionTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/run/branch/selection/golden/RunBranchSelectionTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         run.branch.selection.golden.RunBranchSelectionTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
