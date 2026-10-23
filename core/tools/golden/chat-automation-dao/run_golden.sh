#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/ChatAutomationDao.kt.
#
# ChatAutomationDao.kt and ChatEntities.kt (TaskEntity, LoopEntity, the other entities and the
# type converters) compile VERBATIM from the app tree — the DAO only needs the model cluster for
# the converter declarations. The platform boundary is Room: platform/MiniRoom.kt executes the
# declared @Query SQL on a real SQLite database over platform/schema.sql, implements @Upsert with
# Room's insert-then-update-by-primary-key algorithm, and returns cold, single-emission Flows.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_lib="$(dirname "$(command -v "$kotlinc_bin")")/../lib"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_lib/kotlinx-serialization-compiler-plugin.jar}"
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
    "$app/data/local/ChatAutomationDao.kt"
    "$app/data/local/ChatEntities.kt"
    "$app/data/local/RunEntity.kt"
    "$app/model/RuntimeIdentity.kt"
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/Citation.kt"
    "$app/model/MessagePersistenceGuard.kt"
    "$app/util/Constants.kt"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/MiniRoom.kt"
    "$here/ChatAutomationTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Generating scenarios..."
python3 "$here/gen_scenarios.py"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$runtime" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/chat/automation/golden/ChatAutomationTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     chat.automation.golden.ChatAutomationTraceDumpKt \
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
