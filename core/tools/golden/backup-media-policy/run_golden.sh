#!/usr/bin/env bash
# Generates or verifies the golden traces for data/NativeBackupMediaPolicy.kt.
#
# NativeBackupMediaPolicy.kt and model/AttachmentMeta.kt compile VERBATIM from the app tree.
# MessageSegment, ToolImageAttachment and StreamingTextDelta live in model/ChatMessage.kt, which
# cannot compile host-side (androidx.compose.runtime.Immutable, UI state), so they are lifted out
# verbatim by agora_extract message-segment rather than retyped. The C++ core must reproduce the
# traces byte for byte (core/tests/backup_media_policy_parity_test.cpp).
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

# The serialization plugin was renamed in the Kotlin 2.x distribution. The app builds with Kotlin
# 2.3.21 and kotlinx-serialization 1.11.0, and the stand must match: 1.11.0 is the first release
# whose Json exposes encodeToString(T)/decodeFromString(String) as reified MEMBERS, which is why
# NativeBackupMediaPolicy.kt calls json.encodeToString(...) without importing the extension. Older
# runtimes do not compile the file verbatim, so matching versions is a requirement, not a choice.
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-}"
if [[ -z "$plugin" ]]; then
    for candidate in \
        "$kotlinc_dir/../lib/kotlin-serialization-compiler-plugin.jar" \
        "$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar"; do
        if [[ -f "$candidate" ]]; then plugin="$candidate"; break; fi
    done
fi

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin (set KOTLINX_SERIALIZATION_PLUGIN)" >&2
    exit 2
fi

classpath="$jars"
classes="$here/build/classes"
fresh="$here/build/golden"
# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting MessageSegment / ToolImageAttachment / StreamingTextDelta verbatim..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" message-segment "$app/model/ChatMessage.kt" "$here/build/MessageSegment.kt"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" \
    "$here/build/MessageSegment.kt" \
    "$here/PlatformStubs.kt" \
    "$app/model/AttachmentMeta.kt" \
    "$app/data/NativeBackupMediaPolicy.kt" \
    "$here/BackupMediaTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/backup/policy/golden/BackupMediaTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$classpath" \
         backup.policy.golden.BackupMediaTraceDumpKt "$scenario" > "$fresh/$name.trace"
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