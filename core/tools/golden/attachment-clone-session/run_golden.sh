#!/usr/bin/env bash
# Generates or verifies the golden traces for data/MessageAttachmentCloneSession.kt.
#
# The policy file is lifted verbatim (only the visibility is widened) and MessageEntity is copied
# out of data/local/ChatEntities.kt verbatim; the Room annotations are the stand-local stubs of the
# native-run-archive stand (Room is a persistence framework: the annotations have no runtime
# behaviour the clone policy can observe).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS must point at the kotlinx jars" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the clone policy and the message entity..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" clone-policy "$app" "$here/build"

sources=(
    "$app/model/ChatMessage.kt"
    "$app/model/AttachmentMeta.kt"
    "$app/model/MessageSource.kt"
    "$app/model/TokenUsage.kt"
    "$app/model/ModelId.kt"
    "$app/model/ContextBudget.kt"
    "$app/model/Citation.kt"
    "$app/model/MessagePersistenceGuard.kt"
    "$app/util/Constants.kt"
    "$here/build/MessageEntity.kt"
    "$here/build/MessageAttachmentCloneSession.kt"
    "$here/platform/RoomAnnotations.kt"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/AttachmentCloneSessionTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$jars" -d "$classes" "${sources[@]}" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/attachment/clone/session/golden/AttachmentCloneSessionTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
rm -rf platform_clone_workspace
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
     attachment.clone.session.golden.AttachmentCloneSessionTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json
rm -rf platform_clone_workspace

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
