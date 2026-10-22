#!/usr/bin/env bash
# Generates or verifies the golden traces for data/local/ChatDaoRuntimeRecovery.kt.
#
# The recovery body, RunBranchSelectionIntegrity.kt, RunEntity.kt, RuntimeIdentity.kt and the
# model files behind the runtime reducer compile VERBATIM from the app tree. MessageEntity,
# MessageStreamCheckpoint and encodeSelectionMap are lifted verbatim by extract_recovery_sources.py.
# The platform boundary is Room: platform/ChatDao.kt transcribes the nine ChatDao queries the body
# calls, platform/MiniRoom.kt executes them on a real SQLite database over platform/schema.sql,
# and platform/DebugLog.kt drops the Android logging sink.
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

echo "Extracting the recovery sources..."
python3 "$here/extract_recovery_sources.py"

sources=(
    "$app/data/local/RunBranchSelectionIntegrity.kt"
    "$app/data/local/RunEntity.kt"
    "$app/model/RuntimeIdentity.kt"
    "$app/model/ConversationRuntimeReducer.kt"
    "$app/model/ConversationCommand.kt"
    "$app/model/RuntimeTransition.kt"
    "$app/model/RunRecoveryPolicy.kt"
    "$app/model/RunState.kt"
    "$app/model/RunEffect.kt"
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
    "$here/build/MessageEntities.kt"
    "$here/build/EncodeSelectionMap.kt"
    "$here/build/ChatDaoRuntimeRecovery.kt"
    "$here/platform/Room.kt"
    "$here/platform/RoomDatabase.kt"
    "$here/platform/MiniRoom.kt"
    "$here/platform/StandChat.kt"
    "$here/platform/ChatDao.kt"
    "$here/platform/StandChatDatabase.kt"
    "$here/platform/DebugLog.kt"
    "$here/RuntimeRecoveryTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$runtime" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/runtime/recovery/golden/RuntimeRecoveryTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     runtime.recovery.golden.RuntimeRecoveryTraceDumpKt \
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
