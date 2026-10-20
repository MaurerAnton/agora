#!/usr/bin/env bash
# Generates or verifies the golden traces for NativeBackupFormat.
# NativeBackupFormat.kt and the shared settings models compile verbatim from the app tree.
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
[[ -n "$jars" ]] || { echo "error: KOTLINX_JARS is required" >&2; exit 2; }

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar" \
    -cp "$jars" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/OpenAiServiceTiers.kt" \
    "$app/data/PromptTemplateItem.kt" \
    "$app/data/EmbeddingModelConfig.kt" \
    "$app/data/SettingsPreferenceSchema.kt" \
    "$app/data/SettingsContracts.kt" \
    "$app/data/NativeBackupFormat.kt" \
    "$here/platform/Preferences.kt" \
    "$here/platform/SecretCrypto.kt" \
    "$here/platform/DebugLog.kt" \
    "$here/NativeBackupFormatTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/backup/format/golden/NativeBackupFormatTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

for scenario in "$here"/scenarios/*.json; do
    stem="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$jars" \
         native.backup.format.golden.NativeBackupFormatTraceDumpKt "$scenario" \
         > "$fresh/$stem.trace"
done

count="$(ls "$fresh"/*.trace | wc -l)"
if [[ "${1:-}" == "--freeze" ]]; then
    rm -f "$here"/golden/*.trace
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
