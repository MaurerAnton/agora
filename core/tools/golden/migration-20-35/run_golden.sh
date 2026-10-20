#!/usr/bin/env bash
# Generates or verifies the golden traces for the SQL-only v20..v35 schema migrations.
#
# data/local/migration/Migration20To21.kt .. Migration34To35.kt compile VERBATIM from the app
# tree. Only the androidx.room/androidx.sqlite platform surface is substituted (platform/). The
# C++ core must reproduce the traces byte for byte (core/tests/migration_20_35_parity_test.cpp).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora/data/local/migration"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$app/Migration20To21.kt" \
    "$app/Migration21To22.kt" \
    "$app/Migration22To23.kt" \
    "$app/Migration23To24.kt" \
    "$app/Migration24To25.kt" \
    "$app/Migration25To26.kt" \
    "$app/Migration26To27.kt" \
    "$app/Migration27To28.kt" \
    "$app/Migration28To29.kt" \
    "$app/Migration29To30.kt" \
    "$app/Migration30To31.kt" \
    "$app/Migration31To32.kt" \
    "$app/Migration32To33.kt" \
    "$app/Migration33To34.kt" \
    "$app/Migration34To35.kt" \
    "$here/platform/RoomMigration.kt" \
    "$here/platform/SupportSqlite.kt" \
    "$here/Migration20To35TraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/migration/twenty/thirtyfive/golden/Migration20To35TraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         migration.twenty.thirtyfive.golden.Migration20To35TraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
