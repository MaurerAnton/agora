#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/MaintenanceDebt.kt.
#
# MaintenanceDebt.kt, RunBranchSelectionIntegrity.kt, data/local/RunEntity.kt and
# model/RuntimeIdentity.kt compile VERBATIM from the app tree. Room's annotation processor is the
# platform boundary: platform/MiniRoom.kt executes the declared @Query SQL on a real SQLite
# database (with Room's insert-then-update-by-primary-key upsert and its per-level transaction
# contract), and platform/schema.sql carries the generated DDL.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_lib="$(dirname "$(command -v "$kotlinc_bin")")/../lib"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx jars)" >&2
    exit 2
fi
runtime="$kotlinc_lib/kotlin-stdlib.jar:$kotlinc_lib/kotlin-reflect.jar:$jars"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

sources=(
    "$app/data/local/MaintenanceDebt.kt"
    "$app/data/local/RunBranchSelectionIntegrity.kt"
    "$app/data/local/RunEntity.kt"
    "$app/model/RuntimeIdentity.kt"
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/MiniRoom.kt"
    "$here/platform/StandChat.kt"
    "$here/platform/StandDebtDatabase.kt"
    "$here/MaintenanceDebtTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -cp "$runtime" -d "$classes" \
    "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/maintenance/debt/golden/MaintenanceDebtTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     maintenance.debt.golden.MaintenanceDebtTraceDumpKt \
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
