#!/usr/bin/env bash
# Generates or verifies the golden traces for data/ClaudeChatImporter.kt.
#
# ClaudeChatImporter.kt, ImportStreams.kt, ImportedCitationProjection.kt, model/Citation.kt,
# model/MessagePersistenceGuard.kt and model/ChatMessage.kt compile VERBATIM from the app tree
# (ChatMessage's compile-only annotations use the citations stand's documented leaf stubs).
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces and fixtures
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
citations_stand="$root/core/tools/golden/citations"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"
coroutines="${COROUTINES_JAR:-}"
if [[ -z "$jars" || -z "$coroutines" ]]; then
    echo "error: KOTLINX_JARS and COROUTINES_JAR must point at the kotlinx jars" >&2
    exit 2
fi
classpath="$jars:$coroutines"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

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
    "$app/data/ImportedCitationProjection.kt"
    "$app/data/ImportStreams.kt"
    "$app/data/ClaudeChatImporter.kt"
    "$citations_stand/stubs/androidx/compose/runtime/Immutable.kt"
    "$citations_stand/stubs/com/mikepenz/markdown/model/State.kt"
    "$here/ClaudeChatImporterTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" "${sources[@]}" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/claude/chat/importer/golden/ClaudeChatImporterTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$classpath" \
     claude.chat.importer.golden.ClaudeChatImporterTraceDumpKt \
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
