#!/usr/bin/env bash
# Generates or verifies the golden traces for the ZIP reader.
#
# The reference is the JDK's java.util.zip.ZipInputStream (the surface the app reads a vendor
# export archive with); the driver builds the archives with ZipOutputStream and reads them back.
# No app source is involved: this is the platform archive layer the importers sit on.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces and fixtures
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"

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

echo "Compiling the Kotlin reference driver..."
"$kotlinc_bin" -cp "$jars" -d "$classes" "$here/ZipReaderTraceDump.kt" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/zip/reader/golden/ZipReaderTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     zip.reader.golden.ZipReaderTraceDumpKt \
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
