#!/usr/bin/env bash
# Generates or verifies the golden traces for NativeConversationGraphSource.kt.
#
# NativeConversationGraphSource.kt compiles VERBATIM from the app tree together with
# NativeBackupFormat.kt (the index decode it drives) and the four-line InputStream.readToEnd()
# lifted by this stand's extractor. The platform types this stand substitutes are
# android.util.JsonReader/JsonWriter/JsonToken/MalformedJsonException and the internal
# StringPool, vendored verbatim from AOSP (vendor/, one documented build-time edit dropping the
# ravenwood annotation lines) and compiled with javac, so the streaming parse, the strict/lenient
# rules, the line/column error arithmetic and the Double.toString emission are Android's own.
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
android_classes="$here/build/android-classes"
vendor_src="$here/build/vendor-src"
read_to_end="$here/build/ReadToEnd.kt"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
python3 "$here/gen_scenarios.py"

echo "Staging the vendored android.util.Json* sources..."
python3 "$here/prepare_vendor.py" "$here/vendor/android-util-json" "$vendor_src"

echo "Compiling the vendored Android JSON platform types..."
javac -nowarn -d "$android_classes" "$vendor_src"/*.java

echo "Extracting InputStream.readToEnd()..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" read-to-end \
    "$app/data/NativeBackupArchive.kt" "$read_to_end"

classpath="$jars:$android_classes"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$android_classes"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" \
    "$app/util/Constants.kt" \
    "$app/model/OpenAiServiceTiers.kt" \
    "$app/data/PromptTemplateItem.kt" \
    "$app/data/EmbeddingModelConfig.kt" \
    "$app/data/SettingsPreferenceSchema.kt" \
    "$app/data/SettingsContracts.kt" \
    "$app/data/NativeBackupFormat.kt" \
    "$read_to_end" \
    "$app/data/NativeConversationGraphSource.kt" \
    "$here/platform/Preferences.kt" \
    "$here/platform/SecretCrypto.kt" \
    "$here/platform/DebugLog.kt" \
    "$here/platform/SettingsManager.kt" \
    "$here/NativeConversationGraphSourceTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/native/conversation/graph/source/golden/NativeConversationGraphSourceTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     native.conversation.graph.source.golden.NativeConversationGraphSourceTraceDumpKt \
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
