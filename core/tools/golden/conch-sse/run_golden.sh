#!/usr/bin/env bash
# Generates or verifies the golden traces for parseConchSseLines.
#
# ConchSseParser.kt compiles VERBATIM from the app tree. Its two dependencies from ShellClient.kt
# (which cannot compile host-side: ShellCrypto/ConchNetwork/DebugLog) are lifted out verbatim by
# extract_shell_prefix.py rather than retyped. The C++ core must reproduce the traces byte for
# byte (core/tests/conch_sse_parity_test.cpp).
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
coroutines="${KOTLINX_COROUTINES_JAR:-$kotlinc_dir/../lib/kotlinx-coroutines-core-jvm.jar}"

if [[ -z "$jars" ]]; then
    echo "error: KOTLINX_JARS is not set (colon-separated paths to the kotlinx-serialization jars)" >&2
    exit 2
fi
if [[ ! -f "$plugin" ]]; then
    echo "error: kotlinx-serialization compiler plugin not found at $plugin (set KOTLINX_SERIALIZATION_PLUGIN)" >&2
    exit 2
fi
if [[ ! -f "$coroutines" ]]; then
    echo "error: kotlinx-coroutines jar not found at $coroutines (set KOTLINX_COROUTINES_JAR)" >&2
    exit 2
fi

classpath="$jars:$coroutines"
classes="$here/build/classes"
fresh="$here/build/golden"
extracted="$here/build/ShellPrefix.kt"
# Scratch lives under build/ so the whole stand's working state is git-ignored as a unit.
rm -rf "$here/build"
mkdir -p "$classes" "$fresh" "$here/build"

echo "Extracting the shell byte-budget helpers verbatim from ShellClient.kt..."
python3 "$here/extract_shell_prefix.py" "$app/util/ShellClient.kt" "$extracted"

echo "Compiling the Kotlin reference sources..."
"$kotlinc_bin" -Xplugin="$plugin" -cp "$classpath" -d "$classes" \
    "$extracted" \
    "$app/tool/ConchSseParser.kt" \
    "$here/ConchSseTraceDump.kt" 2>&1 | grep -v "^warning:" || true
[[ -f "$classes/conch/sse/golden/ConchSseTraceDumpKt.class" ]] || { echo "error: harness classes missing" >&2; exit 2; }

for scenario in "$here"/scenarios/*.json; do
    name="$(basename "$scenario" .json)"
    java -cp "$classes:$kotlinc_dir/../lib/kotlin-stdlib.jar:$classpath" \
         conch.sse.golden.ConchSseTraceDumpKt "$scenario" "$here/build" > "$fresh/$name.trace"
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
