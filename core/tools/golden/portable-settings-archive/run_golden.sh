#!/usr/bin/env bash
# Generates or verifies the golden traces for PortableSettingsArchive (export + restore).
#
# data/PortableSettingsArchive.kt compiles VERBATIM from the app tree, together with
# SettingsContracts.kt, NativeBackupFormat.kt, CustomProviderConfig.kt,
# CustomProviderNamePolicy.kt, SettingsDefaults.kt, EmbeddingModelConfig.kt,
# ThinkingControl.kt and OpenAiServiceTiers.kt. The identity policy inside
# CustomProviderIdentity.kt is lifted byte-for-byte by the provider-identity stand's
# documented extractor (`agora_extract provider-identity`). DataStore SettingsManager is replaced by an
# in-memory recording double (platform/) that serves scenario-seeded flows and records saves.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"
identity_stand="$root/core/tools/golden/provider-identity"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
jars="${KOTLINX_JARS:-}"
coroutines="${COROUTINES_JAR:-}"
if [[ -z "$jars" || -z "$coroutines" ]]; then
    echo "error: KOTLINX_JARS and COROUTINES_JAR must point at the kotlinx jars" >&2
    exit 2
fi
classpath="$jars:$coroutines"

plugin="$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar"
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
identity="$here/build/IdentityPolicy.kt"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Extracting the identity policy from CustomProviderIdentity.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" provider-identity "$app/data/CustomProviderIdentity.kt" "$identity"

sources=(
    "$app/util/Constants.kt"
    "$app/model/OpenAiServiceTiers.kt"
    "$app/model/ThinkingControl.kt"
    "$app/data/PromptTemplateItem.kt"
    "$app/data/EmbeddingModelConfig.kt"
    "$app/data/SettingsPreferenceSchema.kt"
    "$app/data/SettingsDefaults.kt"
    "$app/data/DefaultSystemPrompt.kt"
    "$app/data/SettingsContracts.kt"
    "$app/data/NativeBackupFormat.kt"
    "$app/data/CustomProviderConfig.kt"
    "$app/data/CustomProviderNamePolicy.kt"
    "$identity"
    "$app/data/PortableSettingsArchive.kt"
    "$here/platform/Preferences.kt"
    "$here/platform/SecretCrypto.kt"
    "$here/platform/DebugLog.kt"
    "$here/platform/SettingsManager.kt"
    "$here/PortableSettingsArchiveTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" "${sources[@]}" 2>&1 \
    | grep -v "^warning:" || true
[[ -f "$classes/portable/settings/archive/golden/PortableSettingsArchiveTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
rm -f platform_font.ttf
java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$classpath" \
     portable.settings.archive.golden.PortableSettingsArchiveTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json
rm -f platform_font.ttf

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
