#!/usr/bin/env bash
# Generates or verifies the golden traces for the v18->v19 forward repair.
#
# data/local/migration/Migration18To19.kt and V18RegenerationEdgeRepairPlanner.kt compile VERBATIM
# from the app tree. Only the androidx.room/androidx.sqlite/android.database platform surface is
# substituted (platform/). The C++ core must reproduce the traces byte for byte
# (core/tests/migration_18_19_parity_test.cpp).
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

python3 "$here/extract_enums.py"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$app/data/local/migration/Migration18To19.kt" \
    "$app/data/local/migration/V18RegenerationEdgeRepairPlanner.kt" \
    "$app/util/Constants.kt" \
    "$here/extracted/MigrationEnums.kt" \
    "$here/platform/AndroidCursor.kt" \
    "$here/platform/RoomMigration.kt" \
    "$here/platform/SupportSqlite.kt" \
    "$here/Migration18To19TraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/migration/eighteen/nineteen/golden/Migration18To19TraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         migration.eighteen.nineteen.golden.Migration18To19TraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
