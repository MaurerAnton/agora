#!/usr/bin/env bash
# Generates or verifies the golden traces for NativeBackupV5Writer.kt + NativeBackupV5Baseline.kt.
#
# Both V5 files and NativeBackupFormat.kt (plus the shared settings models it needs) compile
# VERBATIM from the app tree — nothing is extracted, every member is internal to this module's
# compilation unit. The platform boundary is Apache Commons Compress: ZipArchiveOutputStream
# writes the golden archives and ZipFile reads them back, so the traces pin the archive at the
# entry level (name, content, CRC, size, method) — the surface the already-pinned zip_reader and
# the importers consume. The compressed byte stream itself belongs to the platform compressor.
#
# Usage:
#   KOTLINX_JARS=... ./run_golden.sh            # verify scenarios against the frozen traces
#   KOTLINX_JARS=... ./run_golden.sh --freeze   # regenerate the frozen traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_lib="$(dirname "$(command -v "$kotlinc_bin")")/../lib"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"
if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to kotlinx + commons jars)" >&2
    exit 2
fi
runtime="$kotlinc_lib/kotlin-stdlib.jar:$kotlinc_lib/kotlin-reflect.jar:$jars"

classes="$here/build/classes"
fresh="$here/build/golden"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$runtime" \
    -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/OpenAiServiceTiers.kt" \
    "$app/data/PromptTemplateItem.kt" \
    "$app/data/EmbeddingModelConfig.kt" \
    "$app/data/SettingsPreferenceSchema.kt" \
    "$app/data/SettingsContracts.kt" \
    "$app/data/NativeBackupFormat.kt" \
    "$app/data/NativeBackupV5Writer.kt" \
    "$app/data/NativeBackupV5Baseline.kt" \
    "$here/platform/Preferences.kt" \
    "$here/platform/SecretCrypto.kt" \
    "$here/platform/DebugLog.kt" \
    "$here/NativeBackupV5TraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/backup/v5/golden/NativeBackupV5TraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
rm -rf v5_workspace
java -cp "$classes:$runtime" \
     native.backup.v5.golden.NativeBackupV5TraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json
rm -rf v5_workspace

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
