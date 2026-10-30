#!/usr/bin/env bash
# Generates or verifies the golden traces for util/ShellClient.kt.
#
# ShellClient.kt, ShellCrypto.kt and ConchHandshake.kt compile VERBATIM from the app tree.
# Platform substitutes (see README):
#   1. platform/ConchNetwork.kt — the OkHttp GET/POST surface with a request log; encrypted
#      responses are sealed with the exact session key ShellClient derived (X25519 symmetry:
#      serverPrivate × clientPublic == clientPrivate × serverPublic) and signed with the real
#      ShellCrypto.responseSignature;
#   2. platform/DebugLog.kt — silent logger;
#   3. platform/Base64.java — vendored android.util.Base64 (same as the shell-crypto stand).
#
# Usage:
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh            # verify
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh --freeze   # regenerate
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

# ShellClient's file/job API suspends: the driver (and runtime) need the coroutines jar.
coroutines="${KOTLINX_COROUTINES_JAR:-}"
if [[ -z "$coroutines" ]]; then
    first="${jars%%:*}"
    coroutines="$(ls "$(dirname "$first")"/kotlinx-coroutines-core-jvm-*.jar 2>/dev/null | head -1 || true)"
fi
if [[ ! -f "$coroutines" ]]; then
    echo "error: kotlinx-coroutines-core-jvm jar not found (set KOTLINX_COROUTINES_JAR)" >&2
    exit 2
fi

classes="$here/build/classes"
fresh="$here/build/golden"
android_classes="$here/build/android-classes"
if [[ -n "${JAVA_HOME:-}" ]]; then
    java_bin="$JAVA_HOME/bin/java"
    javac_bin="$JAVA_HOME/bin/javac"
else
    java_bin="java"
    javac_bin="javac"
fi
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars:$coroutines:$android_classes"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$android_classes"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" shell-client "$here"

echo "Compiling the vendored Android Base64 platform type..."
"$javac_bin" -nowarn -d "$android_classes" "$here/platform/Base64.java"

sources=(
    "$app/util/ShellClient.kt"
    "$app/util/ShellCrypto.kt"
    "$app/util/ConchHandshake.kt"
    "$here/platform/ConchNetwork.kt"
    "$here/platform/DebugLog.kt"
    "$here/ShellClientTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars:$coroutines:$android_classes"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/client/golden/ShellClientTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
"$java_bin" -cp "$classes:$runtime" \
     shell.client.golden.ShellClientTraceDumpKt \
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
