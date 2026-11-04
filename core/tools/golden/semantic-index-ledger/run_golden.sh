#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/SemanticIndexLedger.kt.
#
# SemanticIndexLedger.kt and util/Constants.kt compile VERBATIM from the app tree; the
# SemanticEmbeddingDao -> SemanticMetadataDao cluster rides along with ChatEntities.kt (the
# data-exporter stand's recipe). Room's annotation processor is the platform boundary:
# platform/MiniRoom.kt executes the declared @Query SQL on a real SQLite database instead of
# Room's generated DAO, and platform/schema.sql carries the generated DDL.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
citations_stand="$root/core/tools/golden/citations"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_lib="$(dirname "$(command -v "$kotlinc_bin")")/../lib"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_lib/kotlinx-serialization-compiler-plugin.jar}"
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi
runtime="$(dirname "$(command -v "$kotlinc_bin")")/../lib/kotlin-stdlib.jar:$kotlinc_lib/kotlin-reflect.jar:$jars"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

sources=(
    "$app/data/local/SemanticIndexLedger.kt"
    "$app/util/Constants.kt"
    # The @Transaction default methods ride SemanticEmbeddingDao -> SemanticMetadataDao; entities
    # and the message model come from the data-exporter stand's recipe (ChatEntities.kt verbatim).
    "$app/data/local/SemanticMetadataDao.kt"
    "$app/data/local/SemanticEmbeddingDao.kt"
    "$app/data/local/SemanticEmbeddingGeneration.kt"
    "$app/data/local/SemanticSourceMetadata.kt"
    "$app/data/local/ChatEntities.kt"
    "$app/data/local/RunEntity.kt"
    "$app/data/local/MaintenanceDebt.kt"
    "$app/data/local/RunBranchSelectionIntegrity.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/RunState.kt"
    "$app/model/RuntimeIdentity.kt"
    "$app/model/ImageArtifact.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
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
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$runtime" -d "$classes" \
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
