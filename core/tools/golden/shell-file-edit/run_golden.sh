#!/usr/bin/env bash
# Generates or verifies the golden traces for the pure file helpers in util/ShellClient.kt.
#
# ShellClient.kt is the Conch shell transport object. The top of the file holds transport-free
# policy: the file-edit matcher/replacer, line counting, SHA-256, line-ending preference and the
# bounded shell-output reader. `agora_extract shell-file-edit` copies those declarations out verbatim;
# only the three private helpers are widened to internal. ShellFileReadResult/shellFileLineCount
# are also extracted here for the cross-check, but their C++ port lives in shell_tools.hpp and is
# pinned by the separate shell-tools stand.
#
# Usage:
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh
#   JAVA_HOME=<jdk17> KOTLINC=<kotlinc> KOTLINX_JARS=<kotlinx jars> ./run_golden.sh --freeze
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
runtime="$kotlinc_dir/../lib/kotlin-stdlib.jar:$kotlinc_dir/../lib/kotlin-reflect.jar:$jars"
rm -rf "$here/build"
mkdir -p "$classes" "$fresh"

echo "Generating scenarios..."
"${AGORA_SCENARIOS:-$root/core/build/agora_scenarios}" shell-file-edit "$here"

echo "Extracting the pure file helpers from ShellClient.kt..."
"${AGORA_EXTRACT:-$root/core/build/agora_extract}" shell-file-edit "$app/util/ShellClient.kt" "$here/build/ShellFileEdit.kt"

sources=(
    "$here/build/ShellFileEdit.kt"
    "$here/ShellFileEditTraceDump.kt"
)
for source in "${sources[@]}"; do
    [[ -f "$source" ]] || { echo "error: missing source $source" >&2; exit 2; }
done

classpath="$jars"
echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xjvm-default=all -java-parameters -Xplugin="$plugin" -cp "$classpath" \
    -d "$classes" "${sources[@]}" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/shell/file/edit/golden/ShellFileEditTraceDumpKt.class" ]] || {
    echo "error: harness classes missing" >&2
    exit 2
}

cd "$here"
java -cp "$classes:$runtime" \
     shell.file.edit.golden.ShellFileEditTraceDumpKt \
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
