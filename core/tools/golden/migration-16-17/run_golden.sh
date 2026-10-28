#!/usr/bin/env bash
# Generates or verifies the golden traces for the v16->v17 SQL migration.
#
# data/local/migration/Migration16To17.kt and LegacyRunBackfillPlanner.kt compile VERBATIM from
# the app tree. Only the androidx.room/androidx.sqlite platform surface is substituted (platform/).
# The C++ core must reproduce the traces byte for byte
# (core/tests/migration_16_17_parity_test.cpp).
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

"${AGORA_EXTRACT:-$root/core/build/agora_extract}" enums-16-17 "$here"

# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -cp "$jars" -d "$classes" \
    "$app/data/local/migration/Migration16To17.kt" \
    "$app/data/local/migration/LegacyRunBackfillPlanner.kt" \
    "$here/extracted/MigrationEnums.kt" \
    "$here/platform/AndroidCursor.kt" \
    "$here/platform/RoomMigration.kt" \
    "$here/platform/SupportSqlite.kt" \
    "$here/Migration16To17TraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/migration/sixteen/seventeen/golden/Migration16To17TraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         migration.sixteen.seventeen.golden.Migration16To17TraceDumpKt "$scenario" > "$fresh/$stem.trace"
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
