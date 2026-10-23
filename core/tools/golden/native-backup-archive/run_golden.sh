#!/usr/bin/env bash
# Generates or verifies the golden traces for NativeBackupArchive.kt.
#
# NativeBackupArchive.kt and its collaborators compile VERBATIM from the app tree; the platform
# substitutes are the Android Context/Uri/ParcelFileDescriptor surface and the DataStore
# SettingsManager double (needed only so SettingsContracts.kt compiles). The archives are
# committed fixtures built byte-exactly by gen_scenarios.py, so both ports read identical bytes.
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
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
graph="$here/build/NativeGraphEntrySource.kt"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Generating scenarios and fixtures..."
python3 "$here/gen_scenarios.py"

echo "Extracting the graph entry-source interface..."
python3 "$here/extract_graph_interface.py" \
    "$app/data/NativeConversationGraphSource.kt" "$graph"

classpath="$jars"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/OpenAiServiceTiers.kt" \
    "$app/data/PromptTemplateItem.kt" \
    "$app/data/EmbeddingModelConfig.kt" \
    "$app/data/SettingsPreferenceSchema.kt" \
    "$app/data/SettingsContracts.kt" \
    "$app/data/NativeBackupFormat.kt" \
    "$app/data/NativeBackupArchive.kt" \
    "$graph" \
    "$here/platform/Preferences.kt" \
    "$here/platform/SecretCrypto.kt" \
    "$here/platform/DebugLog.kt" \
    "$here/platform/SettingsManager.kt" \
    "$here/platform/Uri.kt" \
    "$here/platform/AndroidContext.kt" \
    "$here/platform/ParcelFileDescriptor.kt" \
    "$here/NativeBackupArchiveTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/backup/archive/golden/NativeBackupArchiveTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$runtime" \
         native.backup.archive.golden.NativeBackupArchiveTraceDumpKt "$scenario" \
         "$here/build" > "$fresh/$stem.trace"
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
