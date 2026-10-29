#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/SemanticIndexLedger.kt.
#
# SemanticIndexLedger.kt and util/Constants.kt compile VERBATIM from the app tree. EmbeddingEntity
# is lifted out of data/local/ChatEntities.kt by `agora_extract embedding-entity`. Room's annotation processor
# is the platform boundary: platform/MiniRoom.kt executes the declared @Query SQL on a real SQLite
# database instead of Room's generated DAO, and platform/schema.sql carries the generated DDL.
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
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
runtime="$(dirname "$(command -v "$kotlinc_bin")")/../lib/kotlin-stdlib.jar:$kotlinc_lib/kotlin-reflect.jar:$jars"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the embedding entity..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" embedding-entity "$app/data/local/ChatEntities.kt" "$here/build/EmbeddingEntity.kt"

sources=(
    "$app/data/local/SemanticIndexLedger.kt"
    "$app/util/Constants.kt"
    "$here/build/EmbeddingEntity.kt"
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/MiniRoom.kt"
    "$here/platform/StandChatDatabase.kt"
    "$here/SemanticIndexLedgerTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -cp "$runtime" -d "$classes" \
    "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/semantic/ledger/golden/SemanticIndexLedgerTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     semantic.ledger.golden.SemanticIndexLedgerTraceDumpKt \
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
