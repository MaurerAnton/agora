#!/usr/bin/env bash
# Generates or verifies the golden traces for data/ExportSpool.kt.
#
# ExportSpool.kt compiles VERBATIM from the app tree; the driver replays writer op sequences and
# reader probes against a real platform spool file (platform_spool.tmp in this directory).
#
# Requirements: kotlinc with the kotlinx-serialization jars on KOTLINX_JARS (the driver uses the
# JSON DSL; ExportSpool.kt itself is pure java.io).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS must point at the kotlinx jars" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

sources=(
    "$app/data/ExportSpool.kt"
    "$here/ExportSpoolTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/export/spool/golden/ExportSpoolTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
rm -f platform_spool.tmp
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     export.spool.golden.ExportSpoolTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json
rm -f platform_spool.tmp

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
