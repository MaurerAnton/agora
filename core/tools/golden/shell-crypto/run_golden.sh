#!/usr/bin/env bash
# Generates or verifies the golden traces for util/ShellCrypto.kt.
#
# ShellCrypto.kt compiles VERBATIM from the app tree. Only the android.util.Base64
# surface is substituted (platform/). The stand exercises every public function
# with deterministic inputs and freezes the output trace.
#
# Usage:
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh            # verify
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh --freeze   # regenerate traces
set -euo pipefail

here="$(cd "$(dirname "$0")" && pwd)"
root="$(cd "$here/../../../.." && pwd)"
app="$root/app/src/main/java/com/newoether/agora"

kotlinc_bin="${KOTLINC:-kotlinc}"
kotlinc_dir="$(dirname "$(command -v "$kotlinc_bin")")"
plugin="${KOTLINX_SERIALIZATION_PLUGIN:-$kotlinc_dir/../lib/kotlinx-serialization-compiler-plugin.jar}"
jars="${KOTLINX_JARS:-}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx + commons jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
android_classes="$here/build/android-classes"
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$android_classes"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" shell-crypto "$here"

echo "Compiling the vendored Android Base64 platform type..."
javac -nowarn -d "$android_classes" "$here/platform/Base64.java"

sources=(
    "$app/util/ShellCrypto.kt"
    "$here/ShellCryptoTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars:$android_classes"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/crypto/golden/ShellCryptoTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     shell.crypto.golden.ShellCryptoTraceDumpKt \
     --batch "$fresh" "$here"/scenarios/*.json

# Mask non-deterministic values in fresh traces
for f in "$fresh"/*.trace; do
    "${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" mask-trace "$f"
done

count="$(ls "$fresh"/*.trace | wc -l)"

if [[ "${1:-}" == "--freeze" ]]; then
    rm -f "$here"/golden/*.trace
    mkdir -p "$here/golden"
    cp "$fresh"/*.trace "$here/golden/"
    # Also mask the golden traces so they match the masked fresh traces
    for f in "$here/golden"/*.trace; do
        "${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" mask-trace "$f"
    done
    echo "Frozen $count traces into $here/golden"
    exit 0
fi

# Mask golden traces before comparison
for f in "$here/golden"/*.trace; do
    "${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" mask-trace "$f"
done

if diff -r "$here/golden" "$fresh" > "$here/build/diff.txt"; then
    echo "PARITY OK: $count traces identical to the frozen golden"
else
    echo "GOLDEN DRIFT:" >&2
    head -80 "$here/build/diff.txt" >&2
    exit 1
fi
