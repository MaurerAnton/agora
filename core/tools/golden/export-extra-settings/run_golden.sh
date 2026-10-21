#!/usr/bin/env bash
# Generates or verifies the golden traces for ExportExtraSettings.restoreLegacyFromJsonObject.
#
# data/ExportExtraSettings.kt, SettingsContracts.kt (ConversationSettings), ThinkingControl.kt
# and OpenAiServiceTiers.kt compile VERBATIM from the app tree. The Android DataStore storage is
# replaced by an in-memory SettingsManager double (platform/) that records the arguments of every
# save method the policy calls.
#
# Usage:
#   KOTLINX_JARS=... COROUTINES_JAR=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... COROUTINES_JAR=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
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

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar" \
    -cp "$classpath" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/OpenAiServiceTiers.kt" \
    "$app/model/ThinkingControl.kt" \
    "$app/data/PromptTemplateItem.kt" \
    "$app/data/EmbeddingModelConfig.kt" \
    "$app/data/SettingsPreferenceSchema.kt" \
    "$app/data/SettingsContracts.kt" \
    "$app/data/ExportExtraSettings.kt" \
    "$here/platform/Preferences.kt" \
    "$here/platform/SecretCrypto.kt" \
    "$here/platform/DebugLog.kt" \
    "$here/platform/SettingsManager.kt" \
    "$here/ExportExtraSettingsTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/export/extra/settings/golden/ExportExtraSettingsTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$classpath" \
     export.extra.settings.golden.ExportExtraSettingsTraceDumpKt \
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
