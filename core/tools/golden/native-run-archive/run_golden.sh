#!/usr/bin/env bash
# Generates or verifies the golden traces for data/NativeRunArchivePolicy.kt.
#
# NativeRunArchivePolicy.kt, model/RuntimeIdentity.kt and data/local/RunEntity.kt compile VERBATIM
# from the app tree. Only RunEntity's Room annotations and the ChatEntity table it names are
# stand-local platform stubs (platform/): they carry no runtime behaviour the policy can observe.
# The C++ core must reproduce the traces byte for byte
# (core/tests/native_run_archive_parity_test.cpp).
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

echo "Compiling the Kotlin reference sources verbatim..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$here/platform/RoomAnnotations.kt" \
    "$here/platform/ChatEntity.kt" \
    "$app/model/RuntimeIdentity.kt" \
    "$app/data/local/RunEntity.kt" \
    "$app/data/NativeRunArchivePolicy.kt" \
    "$here/NativeRunArchiveTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/run/archive/golden/NativeRunArchiveTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         native.run.archive.golden.NativeRunArchiveTraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
